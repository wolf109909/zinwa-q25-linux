# Q25 native-systemd hardware validation — 2026-11-08 UTC

## Scope and safety

Hardware lane based on `acc6a04`. No userdata/boot partition was written, no
`build/flash.sh` was run, and the shared `~/q25-work/linux-7.3` tree/configuration
was not rebuilt or edited. Module output is confined to this worktree. Boot/DT
installation is reserved for the supervisor.

**This is a partial acceptance record.** IPv6 key SSH now works and native
systemd plus the running kernel were verified. Live USB DHCP and modem startup
failures have specific causes and repository fixes below. Applying those fixes
was blocked by broken admin authentication. None of the changed modules or
system configuration files has been installed on-device, and no reboot was
performed. Only the approved account lockout reset and SSH public-key addition
were made on-device.

Host timestamps are 2026-11-08 UTC. The handset clock is wrong: its journal
shows 2026-10-19 CST (UTC+8). Do not compare those clocks as elapsed time.

## Access and native-boot evidence

The initial image-default password was wrong; the supervisor later supplied
the user's changed password. An early retry after only 8m37s was a worker
mistake and failed during the likely lockout interval. No more attempts were
made until a supervisor-approved retry at **18:34:00 UTC**, verified with
`date -u`, more than 12 minutes after the preceding failed attempt. It succeeded.
The user-owned `faillock --user alarm --reset` succeeded; Windows
`id_ed25519.pub` was appended to `~alarm/.ssh/authorized_keys` without replacing
existing keys (directory 0700, file 0600). Subsequent SSH used the key, not
password retries.

Arch's default faillock policy (`deny=3`, `unlock_time=600`) is hostile to a
four-digit-PIN phone workflow: a few failed SSH attempts can also deny the
owner's lock-screen login. The initial lockout was suspected, not independently
confirmed from its failure records; subsequent `faillock --user alarm` checks
were empty. No faillock configuration was changed. Policy changes are reserved
for the supervisor, and no further password-login attempts are planned.

Commands and observed output:

```text
$ uname -r
7.3.3-cachyos-q25
$ ps -p 1 -o pid,comm,args
1 systemd /usr/lib/systemd/systemd
$ systemctl is-system-running
running
$ systemctl --failed --no-pager
0 loaded units listed.
$ id
uid=1000(alarm) ... groups=...998(wheel)
```

A clean failed-units list does not mean the hardware is working: mdinit is in
an auto-restart loop, and MM ignores its readiness pre-command failure.

### Admin blocker and image fix

`sudo` denied the wheel user because `/etc/sudoers.d` was empty. With supervisor
approval two bounded pkexec attempts were made: Paramiko with an allocated PTY,
then Windows OpenSSH `-tt`. Both reached the password prompt and failed with:

```text
polkit-agent-helper-1: error response to PolicyKit daemon:
GDBus.Error:org.freedesktop.PolicyKit1.Error.Failed: No session for cookie
```

The second attempt had a real `/dev/pts/0` and valid logind session 13. The SSH
PAM include chain reaches `pam_systemd.so` and `loginctl` reports an active
remote user session. `faillock --user alarm` remained empty after both attempts.
This is not evidence of a bad password or a missing PAM session; no speculative
PAM/polkit edits were made. Further admin attempts were stopped as directed.

The repository now supplies `rootfs/etc/sudoers.d/10-wheel` with the standard
password-required `%wheel ALL=(ALL:ALL) ALL` rule (not NOPASSWD). `visudo -cf`
reports `parsed OK`. Its local mode is 0440; because Git only preserves the
executable bit, the image worker must install/chmod it to 0440 in the image.
That worker has been notified. The live file was **not** created.

## USB DHCP evidence

Windows commands (interface index is specific to this USB enumeration):

```powershell
Get-NetIPConfiguration -InterfaceIndex 43
Get-NetIPAddress -InterfaceIndex 43 -AddressFamily IPv4
ping -n 1 -w 2000 10.15.19.82
ping -6 -n 2 -w 1000 ff02::1%43
Get-NetNeighbor -InterfaceIndex 43 -AddressFamily IPv6
```

Observed:

```text
UsbNcm Host Device #4: Up, ifIndex 43
IPv4Address: 169.254.37.106/16 (APIPA); no 10.15.19.* address
10.15.19.82: Request timed out
IPv6 neighbor: fe80::8033:c4ff:fe62:3a75, MAC 82-33-C4-62-3A-75
```

A supervisor-approved attempt to assign `10.15.19.100/24` temporarily with
`New-NetIPAddress ... -PolicyStore ActiveStore` required elevation; its UAC
prompt was canceled. No host address was changed. IPv6 SSH avoided that issue.

### Confirmed cause and repository fix

Live commands: `ip -br a`, `nmcli device`, `nmcli connection`,
`udevadm info /sys/class/net/usb0`, and
`cat /usr/lib/udev/rules.d/85-nm-unmanaged.rules`.

```text
usb0  UP  10.15.19.82/24 fe80::8033:c4ff:fe62:3a75/64
usb0  ethernet  unmanaged  --
USB network  4a2f1c7e-9a55-4c1f-9a12-0f2b5f6a2501  ethernet  --
E: DEVTYPE=gadget
E: ID_NET_DRIVER=g_ether
E: NM_UNMANAGED=1
```

The stock NM rule explicitly sets `NM_UNMANAGED=1` for `DEVTYPE=gadget`.
Thus the shared profile is loaded but cannot autoconnect or start its DHCP
server. This is not a missing USB host driver, bad IPv4 prefix, or an absent
profile. `90-q25.conf` does not mark usb0 unmanaged. The earlier keyfile-mode
hypothesis was not the observed live failure; the image still needs mode 0600
on NM keyfiles and the image worker has been informed.

`rootfs/etc/udev/rules.d/86-q25-usb-gadget.rules` overrides that stock default
only for `usb0` gadget add/change/move events, setting `NM_UNMANAGED=0`.
`udevadm verify` reports one file checked, one success, zero failures. This rule
has **not** been installed or triggered live. After installation, reload rules,
trigger usb0's change event and verify the shared profile autoconnects; inspect
NM/dnsmasq logs if it does not. Acceptance still requires an actual DHCP lease
on Windows after re-enumeration, with no manually assigned IPv4 address.

## Display and keyboard backlights

### Broken wiring and changes

* The panel had no `backlight` phandle. Its callbacks therefore never invoked
  the real backlight provider on DPMS-off.
* The real provider is **OCP2131 at `lcd-bias@3e`**, registered as
  `/sys/class/backlight/backlight` by `ocp2131_drv.c`. It already calls
  `backlight_get_brightness()`, which makes `bl_power=4` and brightness zero
  reach its off path. The old PWM node was deliberately removed in `36e9711`;
  reintroducing it would attach the wrong provider.
* The DTS now links the panel to `i2c_lcd_bias_mtk`. The panel uses
  `drm_panel_of_backlight()` so DRM core owns reference handling and the
  backlight enable/disable sequence.
* Keyboard LEDs already have the standard `q25:white:kbd_backlight` name.
  The old display notifier was compiled only for vendor DRM_MEDIATEK_V2, not
  the running upstream DRM path. The driver now uses the standard DRM panel
  follower API and a DT `panel` link. Panel disable masks the LED output to
  zero without turning off keyboard/trackpad input. Re-enable restores the
  requested LED-class brightness. Writes while blanked remain masked, and
  system resume no longer unconditionally sets brightness to 255.
* The legacy proc control and keyboard brightness hotkeys are routed through
  the same LED-class setter, so neither can bypass DPMS masking. The LED device
  is registered before the keyboard IRQ is requested. No new userspace
  controller, notifier daemon or private API was added.

The read-only kernel's `mtk_dsi.c` has the standard bridge atomic
pre-enable/enable/disable/post-disable callbacks and panel bridge attachment.
There is no demonstrated need to patch Mediatek DRM itself yet.

### Host checks

```sh
python3 devicetree/tests/test_mt8781_zinwa_q25.py -v
python3 tests/test_q25_hardware_defaults.py -v
for d in display keyboard sensors; do
    make -C "$HOME/q25-work/linux-7.3" M="$PWD/drivers/$d" \
        ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j4 modules
done
modinfo -F vermagic drivers/display/panel-q20-hd720-lcm-dsi-vdo.ko
modinfo -F vermagic drivers/keyboard/bbqX0kbd.ko
modinfo -F vermagic drivers/sensors/q25_sensors.ko
```

Results:

```text
test_keyboard_follows_the_dsi_panel ... ok
test_panel_uses_real_ocp2131_backlight ... ok
test_sensor_has_explicit_uncalibrated_fallback ... ok
Ran 3 DT tests ... OK
test_mdinit_does_not_run_host_sleep_in_android_chroot ... ok
test_usb_gadget_override_has_valid_syntax ... ok
test_wheel_sudo_requires_password ... ok
Ran 3 rootfs-default tests ... OK
All three module build commands completed successfully.
Each vermagic: 7.3.3-cachyos-q25 SMP preempt mod_unload aarch64
```

The DT tests preprocess/compile the real DTS in a temporary directory and
compare resolved phandles in the DTB, not just strings in source. The rootfs
tests guard the failing mdinit pre-command and verify rule/sudoers syntax with
udevadm/visudo; they do not simulate a cold boot or a DHCP lease. A separate dtc run
succeeded with pre-existing address/interrupt/bus warnings. The shared
`.config`, `Image.gz` and DTB SHA-256 values were captured before building;
`sha256sum -c /tmp/q25-hw-kernel-before.sha256` reported **OK** for all three
files afterward. `git diff --check` passed. Rebuilding all three directories
with `W=1` also passed; the only warning was the pre-existing unused `ret` in
`drivers/keyboard/q20_keyboard.c:292`, an unchanged alternate keyboard driver.

### Required device checks (not yet run)

After the new embedded DT and modules are installed, check the KWin session's
power-button/PowerDevil DPMS path (locking alone need not mean DPMS-off). Record
`/sys/kernel/debug/dri/*/state`, backlight `bl_power`, `brightness` and
`actual_brightness` before/after off/on; confirm the physical display is dark
and resumes correctly. Exercise at least two cycles and a non-default keyboard
brightness. While off, writing a nonzero keyboard brightness must not illuminate
it; waking must restore the requested value. LED sysfs brightness is the
requested level, not a physical-light measurement. Check UPower's
`org.freedesktop.UPower.KbdBacklight` API as well. No such live success is claimed.

## Accelerometer and iio-sensor-proxy

The SC7A20 already exposes IIO raw XYZ and scale `0.009576806` m/s² per count.
The missing driver metadata was `in_accel_mount_matrix`. It now uses
`iio_read_mount_matrix()` and `IIO_MOUNT_MATRIX` ext_info. The driver binds to
the DT `zinwa,q25-sensors` node; older DTs still get the existing platform-device
fallback and IIO's identity default. The raw sensor samples are not transformed
in-kernel, avoiding applying the mount matrix twice.

### Orientation provenance — explicitly UNCALIBRATED

The following bounded searches returned **no** accelerometer direction/remap:

```sh
grep -inE 'direction|accel|gsensor|sensorhub|sc7a|mount-matrix|axis.*map' \
  ~/q25-work/stock/stock.dts ~/q25-work/q25-live.dts
find ~/q25-work/q25_vendor_ramdisk -maxdepth 5 -type f \
  \( -iname '*sensor*' -o -iname '*accel*' -o -iname '*sc7*' \)
```

`stock.dts` has 6,955 lines and `q25-live.dts` 8,402; the only sensor-name match
was the unrelated camera `mediatek,imgsensor`. The extracted vendor ramdisk's
`vendor/` directory is empty. The available factory `cust.dtsi` contains only
its relative include-path text. There is no stock direction number to translate
through MTK's `hwmsen_get_convert()` sign/axis table in these available files.
After login was restored, the live vendor partition's
`/var/lib/q25/android/vendor/etc/sensors/` was inspected: it contains only
`hals.conf`, naming `android.hardware.sensors@2.X-subhal-mediatek.so`, and no
axis/direction configuration. No stock-derived matrix can be justified from
this evidence.

Per supervisor approval, the DTS carries **identity as an explicitly
UNCALIBRATED fallback**, not as a measured board orientation. Face-up Z sign and
upright/left/right gravity readings still need measurement. A face-up stationary
sample should yield approximately +9.81 m/s² on Z after the matrix. Do not claim
correct auto-rotation until `monitor-sensor` and KWin agree with physical poses.
This hardware driver exposes an accelerometer, magnetometer and ALS/proximity;
it does not expose or promise a separate gyroscope.

`iio-sensor-proxy` is absent from the original repository package list; the
image worker was asked to include it. The current staged ext4 rootfs already has
3.9-1. Its stock `80-iio-sensor-proxy.rules` recognizes the raw XYZ attributes,
tags the IIO device for systemd, and requests `iio-sensor-proxy.service`; the
service is static and does not need `systemctl enable`.

### Live baseline (old module and DT)

```text
$ systemctl status iio-sensor-proxy --no-pager
Active: active (running), started during this boot
$ cat /sys/bus/iio/devices/iio:device6/name
sc7a20
$ cat /sys/bus/iio/devices/iio:device6/in_accel_{x,y,z}_raw
-26
31
1060
$ cat /sys/bus/iio/devices/iio:device6/in_accel_scale
0.009576806
$ timeout 4 monitor-sensor --accel
+++ iio-sensor-proxy appeared
Failed to claim accelerometer: ... AccessDenied: Not Authorized: Sensor claim not allowed
```

Z was positive, approximately +10.15 m/s²; the unattended phone's physical pose
was not confirmed, so this is **not** a face-up calibration. The raw device and
proxy exist, but the remote SSH sensor claim was denied by policy. Verify
`monitor-sensor --accel` from the active local Plasma session, then test actual
rotation after the new matrix-capable module/DT are installed. No rotated GUI
or correct orientation result has been observed.

A standard local hwdb `ACCEL_MOUNT_MATRIX` override is available while
calibrating: inspect the IIO device's parent modalias with `udevadm info -a`,
match the `sensor:modalias:...` lookup used by `/usr/lib/udev/rules.d/60-sensor.rules`
in `/etc/udev/hwdb.d/61-q25-sensor-local.hwdb`, and supply the measured nine-value
matrix. Run `systemd-hwdb update`, trigger IIO uevents and restart
`iio-sensor-proxy`. The DT node changes the parent modalias from
`platform:q25-sensors` to an OF modalias; use the actual lookup, not a guessed
DMI identifier. Do not install an unmeasured axis correction.

## Cellular — startup failure found; LTE not yet validated

Live commands: `systemctl status ModemManager q25-ccci@ccci_mdinit
q25-ccci@ccci_rpcd --no-pager`, `systemctl cat q25-ccci@ccci_mdinit`,
`ls -l /dev/ttyC0`, `mmcli -L` and `ip -br a`.

```text
q25-ccci@ccci_rpcd: active (running) since boot
q25-ccci@ccci_mdinit: activating (auto-restart), Result: exit-code
ExecStartPre=/usr/bin/sleep 1 (code=exited, status=203/EXEC)
ModemManager: active (running)
ExecStartPre=/usr/lib/q25/q25-modem-ready (code=exited, status=1/FAILURE)
[base-manager] couldn't check support for device 'q25-md1': not supported by any plugin
/dev/ttyC0: character device, major 505, minor 0
mmcli -L: No modems were found
ccmni0..20: DOWN, no IPv4 addresses
wlan0: DOWN
```

The template has `RootDirectory=/var/lib/q25/android`; its mdinit drop-in's
`ExecStartPre=+/usr/bin/sleep 1` still runs inside that Android root, where the
host `/usr/bin/sleep` does not exist. The `+` prefix does not escape this chroot.
Thus mdinit never executes, explaining the readiness timeout and no modem.
Reading `/sys/kernel/ccci/boot` as alarm was denied; no MD1 READY claim is made.

Per supervisor direction, the drop-in now removes the failing arbitrary delay
rather than adding a host-side helper. `Requires`, `After` and `BindsTo` for
rpcd remain. There is no evidence a one-second delay is necessary; whether
spawn ordering alone is sufficient must be confirmed on the next real boot.
The modem readiness helper and plugin are unchanged, and no process was made
to compete for `ccci_monitor`.

Because admin access was unavailable, the corrected drop-in was not installed
and neither daemon nor MM was restarted. The previous
`packages/modemmanager-q25/VALIDATION.md` still only demonstrates the legacy
chroot path. Remaining acceptance is **not passed**: cold-start MD1 READY;
`mmcli -L` discovery without manual restart; standard NM profile with APN
`ctnet` and initial-EPS APN `ctnet`; registered/attached LTE with IPv4 /32 on the
dynamically selected ccmni device; HTTPS bound to that device with Wi-Fi not
providing the tested route; disconnect/reconnect and reboot autoconnect; Plasma
cellular page presence. Redact SIM identifiers from subsequent published logs.
