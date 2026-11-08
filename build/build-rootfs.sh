#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the Zinwa Q25 Arch Linux ARM root filesystem from scratch.
#
#   sudo build/build-rootfs.sh [stage...]
#
# Stages (default: all, in order):
#   bootstrap  pacstrap the packages from build/packages.txt into $ROOT
#   packages   install the Q25 packages built by build-packages.sh ($OUT/packages)
#   overlay    install rootfs/ overlay, firmware, kernel modules, Q25 tools
#   configure  users, locale, services, keyring
#   image      make out/q25-rootfs.img (btrfs, default @) + sparse chunks
#              requires host btrfs-progs 7.x (--rootdir/--subvol/--compress)
#
# Environment:
#   ROOT      rootfs staging dir           (default ~/q25-work/rootfs)
#   OUT       output directory              (default <repo>/out)
#   KSTAGE    kernel staging dir from build-kernel.sh (default ~/q25-work/kstage)
#   SSH_KEY   public key added to root + user authorized_keys (optional)
#   Q25_USER  default user name             (default alarm)
#   Q25_PASSWORD initial user password/PIN  (default $Q25_USER; numeric recommended)
#   Q25_APN   carrier APN (e.g. ctnet): ships a NetworkManager "mobile" profile
#             with the initial-EPS (attach) APN set, which this modem needs before
#             it can register. Unset: create it later (packages/modemmanager-q25)
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
HOME_DIR=$(getent passwd "${SUDO_USER:-$USER}" | cut -d: -f6)
ROOT=${ROOT:-$HOME_DIR/q25-work/rootfs}
OUT=${OUT:-$REPO/out}
KSTAGE=${KSTAGE:-$HOME_DIR/q25-work/kstage}
Q25_USER=${Q25_USER:-alarm}
Q25_PASSWORD=${Q25_PASSWORD:-$Q25_USER}

[ "$(id -u)" = 0 ] || { echo "run as root (sudo)"; exit 1; }

log() { printf '\033[1;32m==> %s\033[0m\n' "$*"; }
in_root() { arch-chroot "$ROOT" "$@"; }

stage_bootstrap() {
    log "bootstrap: pacstrap into $ROOT"
    mkdir -p "$ROOT"
    mapfile -t PKGS < <(grep -v '^\s*#' "$REPO/build/packages.txt" | grep -v '^\s*$')
    SNAP_PAC_SKIP=1 pacstrap -C "$REPO/build/pacman-build.conf" -c -G -M "$ROOT" "${PKGS[@]}"
}

stage_packages() {
    local pkgs=("$OUT"/packages/*.pkg.tar.*)
    [ -e "${pkgs[0]}" ] || { echo "no packages in $OUT/packages: run 'sudo build/build-packages.sh' first"; exit 1; }
    log "packages: ${pkgs[*]##*/}"
    # host pacman (like pacstrap): the staging dir is not a mountpoint, so
    # pacman inside arch-chroot cannot do its free-space check
    SNAP_PAC_SKIP=1 pacman --config "$REPO/build/pacman-build.conf" --root "$ROOT" \
        --noconfirm --needed --ask 4 -U "${pkgs[@]}"   # 4 = yes, replace conflicting pkgs (phoc -> phoc-q25)
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

configure_archlinuxcn_keyring() (
    # Trust-on-first-use exception is scoped to one package from one repository.
    # Never relax the shipped /etc/pacman.conf's signature policy.
    if ! in_root pacman -Q archlinuxcn-keyring >/dev/null 2>&1; then
        local conf
        # arch-chroot mounts a fresh /tmp, so keep this outside it.
        conf=$(mktemp "$ROOT/etc/pacman.d/q25-archlinuxcn.XXXXXX.conf")
        trap 'rm -f "$conf"' EXIT
        cat > "$conf" <<'EOF'
[options]
Architecture = aarch64
SigLevel = Required DatabaseOptional
[archlinuxcn]
SigLevel = Never
Server = https://mirrors.cernet.edu.cn/archlinuxcn/$arch
EOF
        in_root env SNAP_PAC_SKIP=1 pacman --config "${conf#"$ROOT"}" \
            --noconfirm --needed -Sy archlinuxcn-keyring
    fi
    in_root pacman-key --populate archlinuxcn
)

stage_configure() {
    log "configure: identity, locale, time"
    echo q25 > "$ROOT/etc/hostname"
    ln -sf /usr/share/zoneinfo/Asia/Shanghai "$ROOT/etc/localtime"
    sed -i 's/^#\(en_US.UTF-8 UTF-8\)/\1/; s/^#\(zh_CN.UTF-8 UTF-8\)/\1/' "$ROOT/etc/locale.gen"
    echo LANG=en_US.UTF-8 > "$ROOT/etc/locale.conf"
    in_root locale-gen >/dev/null
    install -Dm644 "$REPO/rootfs/etc/pacman.d/mirrorlist" "$ROOT/etc/pacman.d/mirrorlist"

    log "configure: users"
    if ! in_root id "$Q25_USER" >/dev/null 2>&1; then
        in_root useradd -m -u 1000 -G wheel,audio,video,input,network,rfkill,uucp,lp -s /bin/bash "$Q25_USER"
    fi
    printf '%s:%s\n' "$Q25_USER" "$Q25_PASSWORD" | in_root chpasswd
    in_root passwd -l root >/dev/null
    if [ -n "${SSH_KEY:-}" ] && [ -f "$SSH_KEY" ]; then
        for h in "$ROOT/root" "$ROOT/home/$Q25_USER"; do
            install -d -m700 "$h/.ssh"
            install -m600 "$SSH_KEY" "$h/.ssh/authorized_keys"
        done
        in_root chown -R "$Q25_USER:$Q25_USER" "/home/$Q25_USER/.ssh"
    fi
    sed -i "s/^User=.*/User=$Q25_USER/" "$ROOT/etc/systemd/system/phosh.service.d/q25.conf"
    if [ -n "${Q25_APN:-}" ]; then
        log "configure: NetworkManager mobile profile (APN $Q25_APN)"
        install -m600 /dev/stdin "$ROOT/etc/NetworkManager/system-connections/mobile.nmconnection" <<EOC
[connection]
id=mobile
uuid=$(cat /proc/sys/kernel/random/uuid)
type=gsm
autoconnect=true

[gsm]
apn=$Q25_APN
initial-eps-bearer-apn=$Q25_APN
initial-eps-bearer-configure=true

[ipv4]
method=auto

[ipv6]
method=disabled
EOC
    fi

    log "configure: GSettings defaults"
    in_root glib-compile-schemas --strict /usr/share/glib-2.0/schemas

    log "configure: services"
    in_root systemctl enable \
        NetworkManager.service bluetooth.service sshd.service systemd-timesyncd.service \
        systemd-resolved.service keyd.service q25-firstboot.service q25-usb-gadget.service \
        q25-android.service ModemManager.service q25-connsys.service q25-bt-vhci.service \
        phosh.service q25-boot-ok.service snapper-timeline.timer snapper-cleanup.timer >/dev/null
    in_root systemctl set-default graphical.target >/dev/null
    in_root systemctl mask systemd-networkd.service systemd-networkd-wait-online.service >/dev/null 2>&1 || true
    ln -sf /run/systemd/resolve/stub-resolv.conf "$ROOT/etc/resolv.conf"

    log "configure: pacman keyring"
    in_root pacman-key --init >/dev/null 2>&1 || true
    in_root pacman-key --populate archlinuxarm >/dev/null 2>&1 || true
    configure_archlinuxcn_keyring
    in_root bash -c 'pkill -f gpg-agent' 2>/dev/null || true
    rm -rf "$ROOT/var/cache/pacman/pkg/"*
}

stage_image() (
    # Subshell owns cleanup; never rearrange the source staging root.
    set -e
    log "image: btrfs (host btrfs-progs 7.x required)"
    mkdir -p "$OUT"
    local used_mb size_mb layout path name img="$OUT/q25-rootfs.img"
    used_mb=$(du -sm --apparent-size "$ROOT" | cut -f1)
    size_mb=$(( used_mb * 13 / 10 + 512 ))
    layout=$(mktemp -d "$OUT/.btrfs-layout.XXXXXX")
    trap 'rm -rf "$layout"' EXIT
    cp -a --reflink=auto "$ROOT" "$layout/@"
    local subvols=(--subvol default:@)
    for pair in home:@home root:@root srv:@srv var/cache:@cache var/log:@log var/tmp:@tmp .snapshots:@snapshots; do
        path=${pair%:*}
        name=${pair#*:}
        mkdir -p "$layout/@/$path"
        mv "$layout/@/$path" "$layout/$name"
        mkdir -p "$layout/@/$path"
        chmod --reference="$layout/$name" "$layout/@/$path"
        chown --reference="$layout/$name" "$layout/@/$path"
        subvols+=(--subvol "rw:$name")
    done
    chmod 750 "$layout/@snapshots" "$layout/@/.snapshots"
    mkdir -p "$layout/q25-boot/good"
    chmod 700 "$layout/q25-boot"
    rm -f "$img"
    truncate -s "${size_mb}M" "$img"
    mkfs.btrfs -q -f -L q25-root --rootdir "$layout" \
        "${subvols[@]}" --compress zstd:1 "$img"
    btrfs check --readonly "$img"
    # --rootdir may enlarge the sparse file to its own allocation estimate.
    log "image: $img ($(stat -c %s "$img") bytes, grows to fill userdata on first boot)"
    python3 "$REPO/build/mksparse-chunks.py" "$img" "$OUT/userdata" 48   # LK fastboot download buffer is 64 MiB
    chown -R "${SUDO_USER:-root}:" "$OUT" 2>/dev/null || true
)

STAGES=("$@")
[ ${#STAGES[@]} -eq 0 ] && STAGES=(bootstrap packages overlay configure image)
for s in "${STAGES[@]}"; do "stage_$s"; done
log "done"
