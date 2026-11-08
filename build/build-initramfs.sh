#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the Q25 boot ramdisk (newc cpio, gzip, padded to exactly 8 MiB).
#
#   build/build-initramfs.sh <rootfs> <out.cpio.gz>
#
# Contents: static busybox (from the rootfs), /init, and e2fsck (+ its shared libraries)
# copied from the Arch rootfs. LK places this ramdisk right after the
# vendor_boot ramdisk; the kernel's DTB hard-codes linux,initrd-end to
# vendor ramdisk + 8 MiB, so the archive is zero-padded to that size.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
ROOT=${1:?rootfs}
OUT=${2:?output}
BUSYBOX=${BUSYBOX:-$ROOT/usr/bin/busybox}   # Arch "busybox" package (static)
MAX=$((8 * 1024 * 1024))

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W"/{bin,sbin,dev,proc,sys,run,tmp,newroot,lib,usr/lib}
ln -s lib "$W/lib64"
install -m755 "$BUSYBOX" "$W/bin/busybox"
install -m755 "$REPO/initramfs/init" "$W/init"

# copy an ELF and its DT_NEEDED closure out of the rootfs
declare -A seen
copy_elf() {
    local f=$1
    [ -n "${seen[$f]:-}" ] && return
    seen[$f]=1
    local src="$ROOT$f"
    [ -L "$src" ] && src="$ROOT$(readlink -f "$src" | sed "s|^$ROOT||")"
    mkdir -p "$W$(dirname "$f")"
    cp -L "$src" "$W$f"
    local lib
    for lib in $(readelf -d "$src" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p'); do
        copy_elf "/usr/lib/$lib"
    done
}
copy_elf /usr/bin/e2fsck
copy_elf /usr/lib/ld-linux-aarch64.so.1
mv "$W/usr/bin/e2fsck" "$W/sbin/e2fsck"
mv "$W"/usr/lib/* "$W/lib/"
rmdir "$W/usr/bin" 2>/dev/null || true
aarch64-linux-gnu-strip --strip-unneeded "$W"/lib/*.so* "$W/sbin/e2fsck" 2>/dev/null || true

# mke2fs.conf-free e2fsck still wants /etc/mtab
mkdir -p "$W/etc"
ln -s /proc/mounts "$W/etc/mtab"

(cd "$W" && find . | LC_ALL=C sort | cpio -o -H newc -R 0:0 --quiet) | gzip -9n > "$OUT"
size=$(stat -c %s "$OUT")
if [ "$size" -gt "$MAX" ]; then
    echo "initramfs is $size bytes, more than the 8 MiB the DTB reserves" >&2
    exit 1
fi
truncate -s $MAX "$OUT"
echo "initramfs: $OUT ($size bytes payload, padded to $MAX)"
