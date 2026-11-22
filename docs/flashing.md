# Flashing and recovery

## Partitions touched

Only `boot_a`, `boot_b` and `userdata` are written. `vendor_boot`, `vbmeta`,
`dtbo`, `lk`, `nvdata`/`nvram` (IMEI, Wi-Fi/BT calibration), `md1img` and every
other partition stay stock. The modem firmware and calibration data are used
from those stock partitions at runtime.

## Fastboot mode

- From Linux on the phone: `systemctl reboot --reboot-argument=bootloader`
- With the phone off or stuck: hold **power + volume up**.

`fastboot devices` should list the phone; `fastboot getvar product` reports
`q20_v12_factory`.

**WSL:** Windows owns the USB device. Use the Windows `fastboot.exe` (Android
platform-tools; `tools/windows/install_fastboot_driver.ps1` installs the USB
driver). `build/flash.sh` translates paths when `FASTBOOT` points to a `.exe`:

```sh
FASTBOOT="/mnt/c/Program Files/platform-tools/fastboot.exe" build/flash.sh
```

## Full image: `build/flash.sh [outdir]`

**Erases userdata.** Writes `boot.img` to both slots, flashes the btrfs root
filesystem as sparse chunks to `userdata` (the bootloader accepts at most 64 MiB
per download), selects slot a and reboots. The first boot grows the filesystem to
the whole partition. `Q25_FLASH_CONFIRM=YES` skips the prompt.

## Kernel updates: `build/deploy-kernel.sh [ssh-target]`

Installs a new kernel on a running phone over SSH (default `root@10.15.19.82`,
USB networking): copies the **complete** module tree of the build, keeps the
previous tree as `/lib/modules/<release>.prev`, runs `depmod`, writes
`$OUT/boot.img` to `boot_a` and verifies it. Reboot afterwards.

Always deploy the kernel and the whole module tree together. The kernel is built
without `MODVERSIONS`, so a stale in-tree module still loads after a config
change and then uses the wrong structure layouts; this has caused memory
corruption before.

## A/B slots and testing kernels

This bootloader does **not** boot slot b after `fastboot set_active b`: the slot
stays marked unbootable and slot a keeps booting. Test kernels therefore go to
`boot_a`, with a known-good image kept on `boot_b` and on the host. The
bootloader does not support `fastboot boot` (RAM boot) either.

To recover from a kernel that does not boot:

```sh
# phone: power + volume up
fastboot flash boot_a known-good-boot.img
fastboot reboot
```

## USB networking

The image configures a USB NCM gadget. The phone is `10.15.19.82`; the host side
gets an address on the same /24 from NetworkManager's shared connection
(`rootfs/etc/NetworkManager/system-connections/q25-usb.nmconnection`).
