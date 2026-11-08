#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build everything needed to flash a Q25, in dependency order:
#   1. kernel + Q25 modules     (build-kernel.sh  -> $KSTAGE)
#   2. Arch rootfs + image      (build-rootfs.sh  -> $ROOT, out/q25-rootfs.img, out/userdata/*.simg)
#   3. boot ramdisk             (build-initramfs.sh, uses busybox/e2fsck from the rootfs)
#   4. boot.img                 (build-boot.sh)
# then flash with build/flash.sh. First time: build/prepare-kernel.sh.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
export KSRC=${KSRC:-$HOME/q25-work/linux-7.3}
export KSTAGE=${KSTAGE:-$HOME/q25-work/kstage}
export ROOT=${ROOT:-$HOME/q25-work/rootfs}
export OUT=${OUT:-$REPO/out}
B="$REPO/build"

[ -d "$KSRC" ] || "$B/prepare-kernel.sh"
"$B/build-kernel.sh"
sudo --preserve-env=KSTAGE,ROOT,OUT,SSH_KEY,Q25_USER "$B/build-rootfs.sh"
"$B/build-initramfs.sh" "$ROOT" "$OUT/initramfs.cpio.gz"
"$B/build-boot.sh" "$KSTAGE/boot/Image.gz" "$OUT/initramfs.cpio.gz" "$OUT/boot.img"
ls -la "$OUT" "$OUT/userdata"
echo "next: build/flash.sh   (erases userdata)"
