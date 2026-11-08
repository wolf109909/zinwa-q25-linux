#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the Q25 boot ramdisk (newc cpio, gzip, padded to exactly 8 MiB).
#
#   build/build-initramfs.sh <rootfs> <out.cpio.gz>
#
# Contents: static busybox (from the rootfs), /init, and btrfs (+ its shared libraries)
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
# Arch merged layout: glibc's ld.so only searches /usr/lib (no ld.so.cache here),
# so libraries live in /usr/lib and /lib, /lib64 are symlinks to it.
mkdir -p "$W"/{bin,sbin,dev,proc,sys,run,tmp,newroot,usr/lib}
ln -s usr/lib "$W/lib"
ln -s usr/lib "$W/lib64"
install -m755 "$BUSYBOX" "$W/bin/busybox"
install -m755 "$REPO/initramfs/init" "$W/init"
install -Dm644 "$REPO/initramfs/boot-root.sh" "$W/usr/lib/q25/boot-root.sh"

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
copy_elf /usr/bin/btrfs
copy_elf /usr/lib/ld-linux-aarch64.so.1
mv "$W/usr/bin/btrfs" "$W/sbin/btrfs"
rmdir "$W/usr/bin" 2>/dev/null || true
aarch64-linux-gnu-strip --strip-unneeded "$W"/usr/lib/*.so* "$W/sbin/btrfs" 2>/dev/null || true

# smoke test: btrfs must actually run from inside the ramdisk layout
if command -v qemu-aarch64-static >/dev/null; then
    qemu-aarch64-static -L "$W" "$W/sbin/btrfs" version >/dev/null 2>&1 \
        || { echo "btrfs does not run inside the initramfs (missing libs?)" >&2; exit 1; }
fi

# Mount table for diagnostic utilities in rescue.
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
