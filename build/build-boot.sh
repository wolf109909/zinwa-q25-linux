#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Build the Zinwa Q25 boot.img (Android boot header v4) from the staged kernel
# and the Q25 boot ramdisk.
#
#   build/build-boot.sh <Image.gz> <initramfs.cpio.gz> <out boot.img>
#
# Layout constraints (see README "Boot chain"):
#  * The kernel carries its own DTB (arch/arm64/kernel/head.S); LK's DTB and
#    the boot.img cmdline are ignored, bootargs come from the Q25 DTS.
#  * LK concatenates vendor_boot's ramdisk (18923935 bytes at 0x66f00000) and
#    this ramdisk; the DTS linux,initrd-end assumes the boot ramdisk is exactly
#    8 MiB, which build-initramfs.sh guarantees.
#  * LK refuses a boot partition without an AVB footer (boot-loops before the
#    kernel). The bootloader is unlocked, so an unsigned hash footer
#    (avbtool add_hash_footer, no key) for the 64 MiB partition is enough.
set -euo pipefail
KERNEL=${1:?Image.gz}
RAMDISK=${2:?initramfs}
OUT=${3:?output boot.img}
PARTITION_SIZE=$((64 * 1024 * 1024))

[ "$(stat -c %s "$RAMDISK")" -eq $((8 * 1024 * 1024)) ] ||
    { echo "ramdisk must be padded to 8 MiB (build-initramfs.sh)" >&2; exit 1; }

mkbootimg --header_version 4 --os_version 12.0.0 --os_patch_level 2026-01 \
    --kernel "$KERNEL" --ramdisk "$RAMDISK" --cmdline "" --output "$OUT"
avbtool add_hash_footer --image "$OUT" --partition_name boot \
    --partition_size "$PARTITION_SIZE" --algorithm NONE
avbtool info_image --image "$OUT" | sed -n '1,12p'
echo "boot: $OUT"
