# SPDX-License-Identifier: GPL-2.0-only
# Sourced by /init. BOOT_TOP is a subvolid=5 mount, never a root snapshot.
# Kept separate so the same BusyBox boot policy can be exercised on loop images.

root_is_bootable() {
    [ -x "$1$INIT" ] || { msg "no $INIT in $1"; return 1; }
    # boot.img is outside btrfs. Fail closed even for a manual snapper rollback.
    if [ ! -s "$1/usr/lib/modules/$KERNEL/modules.dep" ] ||
       ! find "$1/usr/lib/modules/$KERNEL" -type f -name '*.ko*' 2>/dev/null | grep -q .; then
        msg "missing modules for running kernel $KERNEL in $1; restore a matching boot.img/root pair"
        return 1
    fi
}

recover_good_snapshot() {
    local number snapshot recovery id
    if [ -e "$BOOT_TOP/q25-boot/recovery-attempt" ]; then
        msg "automatic recovery already attempted; refusing a recovery loop"
        return 1
    fi
    for number in $(ls "$BOOT_TOP/q25-boot/good" 2>/dev/null | sort -rn); do
        case "$number" in ''|*[!0-9]*) continue ;; esac
        snapshot="$BOOT_TOP/@snapshots/$number/snapshot"
        [ -d "$snapshot" ] || continue
        root_is_bootable "$snapshot" || continue
        recovery="$BOOT_TOP/q25-boot/recovery-$number-$(date +%s)"
        # Persist BEFORE changing the default, including across power loss.
        echo "$number" > "$BOOT_TOP/q25-boot/recovery-attempt" || return 1
        sync
        btrfs subvolume snapshot "$snapshot" "$recovery" || return 1
        id=$(btrfs inspect-internal rootid "$recovery") || return 1
        btrfs subvolume set-default "$id" "$BOOT_TOP" || return 1
        sync
        msg "recovering good Snapper snapshot $number as writable default subvolume $id"
        return 0
    done
    msg "no good Snapper checkpoint with modules for $KERNEL"
    return 1
}

prepare_default_boot() {
    local n
    n=$(cat "$BOOT_TOP/q25-boot/boot-count" 2>/dev/null || echo 0)
    # Clamp even corrupted/oversized numeric counters without shell overflow.
    case "$n" in ''|*[!0-9]*) n=0 ;; 0|1|2) ;; *) n=3 ;; esac
    if [ "$n" -ge 3 ]; then
        recover_good_snapshot || return 1
        n=0
    fi
    # No subvol= or subvolid=: snapper rollback changes the DEFAULT subvolume.
    mount -t btrfs -o "rw,noatime,compress=zstd:1${ROOTFLAGS:+,$ROOTFLAGS}" "$ROOTDEV" "$NEWROOT" || return 1
    if ! root_is_bootable "$NEWROOT"; then
        umount "$NEWROOT"
        return 1
    fi
    if ! echo $((n + 1)) > "$BOOT_TOP/q25-boot/boot-count"; then
        umount "$NEWROOT"
        return 1
    fi
    sync
}
