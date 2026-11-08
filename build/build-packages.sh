#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the Q25 Arch packages under packages/ (aarch64) with makepkg inside a
# clean Arch Linux ARM build chroot (qemu-user binfmt on x86_64 hosts).
#
#   sudo build/build-packages.sh [package-dir...]     (default: all of packages/*)
#
# Output: $OUT/packages/*.pkg.tar.* (installed into the image by build-rootfs.sh)
# Environment: BUILDROOT (default ~/q25-work/pkgbuild-root), OUT (default <repo>/out)
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
HOME_DIR=$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)
BUILDROOT=${BUILDROOT:-$HOME_DIR/q25-work/pkgbuild-root}
OUT=${OUT:-$REPO/out}

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }
log() { printf '\033[1;32m==> %s\033[0m\n' "$*"; }

if [ ! -x "$BUILDROOT/usr/bin/makepkg" ]; then
    log "creating build chroot $BUILDROOT"
    mkdir -p "$BUILDROOT"
    pacstrap -C "$REPO/build/pacman-build.conf" -c -G -M "$BUILDROOT" base base-devel
    arch-chroot "$BUILDROOT" useradd -m builder 2>/dev/null || true
    echo 'builder ALL=(ALL) NOPASSWD: ALL' > "$BUILDROOT/etc/sudoers.d/builder"
    cp "$REPO/build/pacman-build.conf" "$BUILDROOT/etc/pacman.conf"
    # the host's pacman-build.conf points CacheDir at the host; use the chroot's own
    sed -i '/^CacheDir/d' "$BUILDROOT/etc/pacman.conf"
fi

mkdir -p "$OUT/packages"
dirs=("$@"); [ ${#dirs[@]} -gt 0 ] || dirs=("$REPO"/packages/*/)
for d in "${dirs[@]}"; do
    d=$(cd "$d" && pwd); name=$(basename "$d")
    log "makepkg $name"
    rm -rf "$BUILDROOT/home/builder/$name"
    cp -a "$d" "$BUILDROOT/home/builder/$name"
    arch-chroot "$BUILDROOT" chown -R builder: "/home/builder/$name"
    # install (make)depends as root: sudo inside the chroot breaks when the build
    # root lives on a nosuid filesystem, so makepkg --syncdeps cannot be used
    deps=$(arch-chroot "$BUILDROOT" runuser -u builder -- bash -c \
        "cd ~/$name && makepkg --printsrcinfo" |
        awk -F' = ' '/^\t(make|check)?depends = /{print $2}' | sort -u | tr '\n' ' ')
    [ -z "$deps" ] || arch-chroot "$BUILDROOT" pacman -Sy --needed --noconfirm $deps
    arch-chroot "$BUILDROOT" runuser -u builder -- bash -c \
        "cd ~/$name && makepkg --noconfirm --cleanbuild --force"
    cp "$BUILDROOT/home/builder/$name"/*.pkg.tar.* "$OUT/packages/"
done
chown -R "${SUDO_USER:-root}:" "$OUT" 2>/dev/null || true
ls -la "$OUT/packages"
