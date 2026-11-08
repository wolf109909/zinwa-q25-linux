#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the Zinwa Q25 Arch Linux ARM root filesystem from scratch.
#
#   sudo build/build-rootfs.sh [stage...]
#
# Stages (default: all, in order):
#   bootstrap  pacstrap the packages from build/packages.txt into $ROOT
#   overlay    install rootfs/ overlay, firmware, kernel modules, Q25 tools
#   configure  users, locale, services, keyring
#   image      make out/q25-rootfs.img (ext4, label q25-root) + sparse chunks
#
# Environment:
#   ROOT      rootfs staging dir           (default ~/q25-work/rootfs)
#   OUT       output directory              (default <repo>/out)
#   KSTAGE    kernel staging dir from build-kernel.sh (default ~/q25-work/kstage)
#   SSH_KEY   public key added to root + user authorized_keys (optional)
#   Q25_USER  default user name             (default alarm, password = user name)
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
HOME_DIR=$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)
ROOT=${ROOT:-$HOME_DIR/q25-work/rootfs}
OUT=${OUT:-$REPO/out}
KSTAGE=${KSTAGE:-$HOME_DIR/q25-work/kstage}
Q25_USER=${Q25_USER:-alarm}

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }

log() { printf '\033[1;32m==> %s\033[0m\n' "$*"; }
in_root() { arch-chroot "$ROOT" "$@"; }

stage_bootstrap() {
    log "bootstrap: pacstrap into $ROOT"
    mkdir -p "$ROOT"
    mapfile -t PKGS < <(grep -v '^\s*#' "$REPO/build/packages.txt" | grep -v '^\s*$')
    pacstrap -C "$REPO/build/pacman-build.conf" -G -M "$ROOT" "${PKGS[@]}"
}

stage_overlay() {
    log "overlay: rootfs/ tree"
    cp -a --no-preserve=ownership "$REPO/rootfs/." "$ROOT/"
    chmod 0440 "$ROOT/etc/sudoers.d/"* 2>/dev/null || true

    log "overlay: Q25 native helpers (src/)"
    local CC="${CROSS_COMPILE:-aarch64-linux-gnu-}gcc"
    install -d "$ROOT/usr/lib/q25"
    for p in q25-wmt-init q25-bt-vhci; do
        # static: independent of the target glibc version and of host sysroot quirks
        $CC -static -O2 -Wall -o "$ROOT/usr/lib/q25/$p" "$REPO/src/$p.c"
    done
    # freestanding (no libc): loads into bionic and glibc processes alike
    $CC -O2 -Wall -shared -fPIC -nostdlib -fno-stack-protector \
        -o "$ROOT/usr/lib/q25/libprop_preload.so" "$REPO/src/libprop_preload.c"
    chmod 755 "$ROOT"/usr/lib/q25/* "$ROOT"/usr/bin/q25-* 2>/dev/null || true
    chmod 644 "$ROOT/usr/lib/q25/libprop_preload.so"

    log "overlay: firmware"
    install -d "$ROOT/usr/lib/firmware"
    cp -a --no-preserve=ownership "$REPO/firmware/." "$ROOT/usr/lib/firmware/"

    if [ -d "$KSTAGE/lib/modules" ]; then
        log "overlay: kernel modules from $KSTAGE"
        rm -rf "$ROOT/usr/lib/modules"
        install -d "$ROOT/usr/lib/modules"
        cp -a --no-preserve=ownership "$KSTAGE/lib/modules/." "$ROOT/usr/lib/modules/"
        for k in "$ROOT"/usr/lib/modules/*; do
            rm -f "$k/build" "$k/source"
            depmod -b "$ROOT/usr" "$(basename "$k")" 2>/dev/null || depmod -b "$ROOT" "$(basename "$k")"
        done
    else
        echo "WARNING: $KSTAGE/lib/modules missing; run build/build-kernel.sh first"
    fi
}

stage_configure() {
    log "configure: identity, locale, time"
    echo q25 > "$ROOT/etc/hostname"
    ln -sf /usr/share/zoneinfo/Asia/Shanghai "$ROOT/etc/localtime"
    sed -i 's/^#\(en_US.UTF-8 UTF-8\)/\1/; s/^#\(zh_CN.UTF-8 UTF-8\)/\1/' "$ROOT/etc/locale.gen"
    echo LANG=en_US.UTF-8 > "$ROOT/etc/locale.conf"
    in_root locale-gen >/dev/null
    cat > "$ROOT/etc/pacman.d/mirrorlist" <<'EOF'
Server = https://mirrors.ustc.edu.cn/archlinuxarm/$arch/$repo
Server = https://mirrors.tuna.tsinghua.edu.cn/archlinuxarm/$arch/$repo
Server = http://mirror.archlinuxarm.org/$arch/$repo
EOF

    log "configure: users"
    if ! in_root id "$Q25_USER" >/dev/null 2>&1; then
        in_root useradd -m -u 1000 -G wheel,audio,video,input,network,rfkill,uucp,lp -s /bin/bash "$Q25_USER"
    fi
    echo "$Q25_USER:$Q25_USER" | in_root chpasswd
    in_root passwd -l root >/dev/null
    if [ -n "${SSH_KEY:-}" ] && [ -f "$SSH_KEY" ]; then
        for h in "$ROOT/root" "$ROOT/home/$Q25_USER"; do
            install -d -m700 "$h/.ssh"
            install -m600 "$SSH_KEY" "$h/.ssh/authorized_keys"
        done
        in_root chown -R "$Q25_USER:$Q25_USER" "/home/$Q25_USER/.ssh"
    fi
    sed -i "s/^User=.*/User=$Q25_USER/" "$ROOT/etc/sddm.conf.d/20-q25.conf"

    log "configure: services"
    in_root systemctl enable \
        NetworkManager.service bluetooth.service sshd.service systemd-timesyncd.service \
        systemd-resolved.service keyd.service q25-firstboot.service q25-usb-gadget.service \
        q25-android.service q25-modemd.service q25-connsys.service q25-bt-vhci.service \
        sddm.service q25-boot-ok.service >/dev/null
    in_root systemctl set-default graphical.target >/dev/null
    in_root systemctl mask systemd-networkd.service systemd-networkd-wait-online.service >/dev/null 2>&1 || true
    ln -sf /run/systemd/resolve/stub-resolv.conf "$ROOT/etc/resolv.conf"

    log "configure: pacman keyring"
    in_root pacman-key --init >/dev/null 2>&1 || true
    in_root pacman-key --populate archlinuxarm >/dev/null 2>&1 || true
    in_root bash -c 'pkill -f gpg-agent' 2>/dev/null || true
    rm -rf "$ROOT/var/cache/pacman/pkg/"*
}

stage_image() {
    log "image: ext4"
    mkdir -p "$OUT"
    local used_mb size_mb img="$OUT/q25-rootfs.img"
    used_mb=$(du -sm --apparent-size "$ROOT" | cut -f1)
    size_mb=$(( used_mb * 13 / 10 + 512 ))
    rm -f "$img"
    truncate -s "${size_mb}M" "$img"
    mkfs.ext4 -q -F -L q25-root -O ^metadata_csum_seed -E lazy_itable_init=1,lazy_journal_init=1 \
        -d "$ROOT" "$img"
    e2fsck -fy "$img" >/dev/null || true
    log "image: $img (${size_mb} MiB, grows to fill userdata on first boot)"
    python3 "$REPO/build/mksparse-chunks.py" "$img" "$OUT/userdata" 120
    chown -R "${SUDO_USER:-root}:" "$OUT" 2>/dev/null || true
}

STAGES=("$@")
[ ${#STAGES[@]} -eq 0 ] && STAGES=(bootstrap overlay configure image)
for s in "${STAGES[@]}"; do "stage_$s"; done
log "done"
