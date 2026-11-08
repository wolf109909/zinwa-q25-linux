#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the Q25 Arch packages under packages/ (aarch64) with makepkg inside a
# clean Arch Linux ARM build chroot (qemu-user binfmt on x86_64 hosts).
#
#   sudo build/build-packages.sh [package-dir...]     (default: all of packages/*)
#
# Output: $OUT/packages/*.pkg.tar.* (installed into the image by build-rootfs.sh)
# Environment: BUILDROOT (default ~/q25-work/pkgbuild-root), OUT (default <repo>/out)
# Host dependency resolution also needs python3, bsdtar and vercmp.
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
# The status plugin needs the patched keyd at build-dependency installation time.
# Keep all unrelated packages in their requested order.
first=(); rest=()
for d in "${dirs[@]}"; do
    d=$(cd "$d" && pwd)
    if [ "$(basename "$d")" = keyd-q25 ]; then first+=("$d"); else rest+=("$d"); fi
done
dirs=("${first[@]}" "${rest[@]}")
missing_deps() {
    local result=0
    arch-chroot "$BUILDROOT" pacman -T "$@" || result=$?
    # pacman deptest uses 127 for unsatisfied dependencies, not a fatal error.
    [ "$result" = 0 ] || [ "$result" = 127 ]
}
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
    if [ -n "$deps" ]; then
        needed=$(missing_deps $deps)
        # keyd-q25 is intentionally not a repository dependency. Never silently
        # substitute stock keyd (which cannot supply authoritative state).
        local_packages=$(python3 "$REPO/build/local-package-deps.py" --arch aarch64 \
            --require keyd-q25 "$OUT/packages" $needed)
        if [ -n "$local_packages" ]; then
            mkdir -p "$BUILDROOT/home/builder/.q25-build-deps"
            local_paths=()
            while IFS= read -r archive; do
                cp "$archive" "$BUILDROOT/home/builder/.q25-build-deps/"
                local_paths+=("/home/builder/.q25-build-deps/$(basename "$archive")")
            done <<< "$local_packages"
            # Accept declared package conflicts (e.g. keyd-q25 replacing keyd),
            # still using libalpm dependency/version/conflict checks.
            arch-chroot "$BUILDROOT" pacman -U --needed --noconfirm --ask=4 "${local_paths[@]}"
        fi
        needed=$(missing_deps $deps)
        [ -z "$needed" ] || arch-chroot "$BUILDROOT" pacman -Sy --needed --noconfirm $needed
    fi
    arch-chroot "$BUILDROOT" runuser -u builder -- bash -c \
        "cd ~/$name && makepkg --noconfirm --cleanbuild --force"
    cp "$BUILDROOT/home/builder/$name"/*.pkg.tar.* "$OUT/packages/"
done
chown -R "${SUDO_USER:-root}:" "$OUT" 2>/dev/null || true
ls -la "$OUT/packages"
