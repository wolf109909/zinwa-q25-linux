# Validation record — 2026-11-08 UTC

## Build and automated checks

* C helper tests: **4/4 passed** (`eif`, `dns`, `apn`,
  `dns-no-local-address`), both on the host and in the AArch64 package build.
* `test-q25-android-setup.py`: **2/2 passed**. An existing mapped library and
  its pathname remain valid during replacement; symlinks are copied intact.
* Shell syntax checks and `git diff --check`: passed.
* Clean AArch64 package build (no incremental build dependency): passed.
  Exact command on this machine, with the PKGBUILD/source files already copied
  to the builder directory and dependencies installed:

  ```sh
  sudo arch-chroot /home/maxsun/q25-work/mm-buildroot \
      runuser -u builder -- bash -c \
      'cd /home/builder/modemmanager-q25 && makepkg --noconfirm -Cf'
  ```

  On a native AArch64 Arch/CachyOS builder, run `makepkg --syncdeps --cleanbuild`
  in this package directory. Build dependencies are declared in `PKGBUILD`.
  Install the resulting package with `pacman -U`; the target also needs the
  repository's kernel module and rootfs modem integration.
* Package contains only the plugin and udev rule (plus package metadata).
  `readelf -d` confirms no RPATH/RUNPATH. The clean-built plugin's SHA-256
  **matches the plugin tested on the phone**:

  ```text
  plugin: 60973f1a85f2a9444c72c72e21bd67b9f9ae1b1f5522769084a4acfb153c691e
  package: ada33171becc54f73c3394a746279d2394c9b2795cdafd28c005bae66cea4755
  ```

  Copied package artifact:
  `/home/maxsun/q25-work/packages/modemmanager-q25-1.24.2-1-aarch64.pkg.tar.xz`.
* The matching live-kernel module was built from an isolated copy of the driver
  sources, **not** the shared integration build output:

  ```sh
  make -C /home/maxsun/q25-work/linux-7.3-running \
      M=/home/maxsun/q25-work/modem-test-drivers/mediatek \
      ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j16 modules
  ```

  The output `eccci/ccci_md_all.ko` has vermagic
  `7.3.3-cachyos-q25-00024-ge50c42ce375d-dirty SMP preempt mod_unload aarch64`.
  Its SHA-256, also verified against the phone, is
  `fe1709eecb9c062bf24ce0acbeb50cf8aa3c7e4c2fda81d7c886eb6ea7d59992`.
  Unrelated pre-existing module-description warnings remain.

## Live handset checks

Target: Q25, matching kernel above, stock Arch Linux ARM
`modemmanager 1.24.2-2`, NetworkManager 1.58.1, MOLY.LR13.R2.MP.V85.3.
Testing used the existing legacy `/rootfs` chroot. No raw boot partition was
written. The old modem module backup remains under
`/rootfs/root/q25-modem-backup` on the handset.

1. `/sys/class/tty/ttyC0` exists beneath the modem platform device;
   `tcgetattr`, polling/reading, and finally `stty -F /dev/ttyC0 -a` work.
   Firmware reaches `md1:4` after reboot. Vendor RPC/MD-init daemons still run.
2. After firmware readiness, stock MM with the plugin discovers modem 0;
   SIM is ready, registration is `home`, packet service `attached`, signal
   quality updates, and NM exposes `ttyC0` as a GSM device.
3. Standard NM profile `q25-lte-test` with APN `ctnet`,
   `gsm.initial-eps-bearer-configure=yes`,
   `gsm.initial-eps-bearer-apn=ctnet`, IPv4 and no roaming activates.
   Logs show `EIAREG=1`, `EAPNACT`, actual EIF parsing/acknowledgement and DNS
   extraction. MM/NM expose IPv4 **/32**, MTU **1400**, and carrier DNS
   `218.2.2.2`, `218.4.4.4`.
4. `nmcli con down` removes the bearer address and routes. Reconnection maps
   EIF `201` to `ccmni1`, then EIF `302` to `ccmni2`; it is not fixed at ccmni0.
5. `curl -4 --noproxy '*' --interface ccmniN --fail -I
   https://www.baidu.com` returns **HTTP 200 with TLS verification**.
   This also passed with Wi-Fi disabled: `wlan0` was unavailable and the sole
   default route was `default dev ccmni2 ... metric 700`. USB provided only a
   local management subnet, not an Internet default route. Wi-Fi was restored.
6. With the NM connection down, `mmcli -m 0 --command='AT+CFUN=1,1'` in MM
   debug mode returned OK, produced a real TTY hangup, and restarted MD1.
   After `q25-modem-ready` succeeded, restarting stock MM, then NM activation,
   restored registration and **HTTP 200 over ccmni0** with a fresh IPv4 /32.
   This tests a controlled firmware reset **plus an explicit MM restart**,
   not unattended modem-crash recovery.
7. Final state: MD1 READY; MM connected/registered/attached; NM and Wi-Fi
   running; `q25-lte-test` connected and autoconnect enabled. Wi-Fi default
   metric 600, LTE default metric 700. No stopped/suspended NM process remains.

## Failures found and resolved during validation

* Shared module output was overwritten by a build for a different kernel
  release. Boot rejected that module. Isolating **both** the kernel tree and
  module output directory, checking vermagic, and replacing the module fixed it.
  Do not install this live-test binary into an image with a different kernel.
* Overriding initial EPS settings initially left MM's inherited CGDCONT-CID
  bearer loader enabled, which asserted. CCCI now disables that incompatible
  loader and uses its own EIAAPN settings path.
* The legacy boot script regenerated its private APN file and left a stale
  external ccmni0 IPv4 /24 and metric-100 route. This contaminated the first
  reset test. Those runtime addresses/routes were removed; a second reset
  starting from clean MM/NM-owned state passed without manual link repair.
  The new rootfs must not start that removed legacy controller.

## Explicitly untested / limitations

* **Native-systemd cold-boot automatic discovery and reboot autoconnect:
  UNTESTED.** The live legacy chroot does not execute the new MM service
  drop-in. Per supervisor direction, no legacy startup harness was added.
  Validate the new rootfs's CCCI ordering and ModemManager enablement on target.
* Automatic recovery without restarting MM after a modem crash/reset, suspend
  and resume, roaming, another SIM/carrier, voice/IMS and IPv6 are untested.
* One default IPv4 bearer, no PAP/CHAP credentials. Unsupported settings are
  rejected. No carrier APN is hardcoded in the plugin.
* Plasma GUI activation was not exercised; GUI-created profiles need the
  standard initial-EPS fields set once, as documented in `README.md`.
* The plugin uses MM's private ABI and is pinned to 1.24.2. Version upgrades
  need a rebuild and retest. Firmware cannot read back EIAAPN settings; only
  successfully configured settings in the current daemon session are cached.

Host build logs remain in `/tmp/q25-mm/`; live debug logs are under the
handset's `/rootfs/var/log/ModemManager-q25*.log`, and the Wi-Fi-off test log is
`/rootfs/root/q25-mm/lte-only-test.log`. These logs can include SIM identifiers
and must not be published without redaction.
