#!/bin/bash
# Exercise the actual initramfs policy on a disposable btrfs loop image.
# sudo build/test-btrfs-boot.sh [aarch64-rootfs]  (optional: qemu + target BusyBox)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
[ "$(id -u)" = 0 ] || { echo 'run as root'; exit 1; }
W=$(mktemp -d)
cleanup() {
    mountpoint -q "$W/root" && umount "$W/root"
    mountpoint -q "$W/top" && umount "$W/top"
    rm -rf "$W"
}
trap cleanup EXIT
truncate -s 512M "$W/test.img"
mkfs.btrfs -q -f "$W/test.img"
mkdir "$W/top" "$W/root"
mount -o loop,subvolid=5 "$W/test.img" "$W/top"
btrfs subvolume create "$W/top/@" >/dev/null
btrfs subvolume create "$W/top/@snapshots" >/dev/null
mkdir -p "$W/top/@/usr/lib/modules/test-kernel" "$W/top/@/usr/lib/systemd" "$W/top/q25-boot/good"
printf '#!/bin/sh\n' > "$W/top/@/usr/lib/systemd/systemd"
chmod +x "$W/top/@/usr/lib/systemd/systemd"
echo test.ko: > "$W/top/@/usr/lib/modules/test-kernel/modules.dep"
echo module > "$W/top/@/usr/lib/modules/test-kernel/test.ko"
for n in 10 20 30; do
    mkdir "$W/top/@snapshots/$n"
    btrfs subvolume snapshot "$W/top/@" "$W/top/@snapshots/$n/snapshot" >/dev/null
    echo test-kernel > "$W/top/q25-boot/good/$n"
done
# Newest checkpoint exists but cannot boot this kernel; older missing ID is stale.
rm -rf "$W/top/@snapshots/30/snapshot/usr/lib/modules"
touch "$W/top/q25-boot/good/40"
for n in 10 20 30; do btrfs property set "$W/top/@snapshots/$n/snapshot" ro true; done
id=$(btrfs inspect-internal rootid "$W/top/@")
btrfs subvolume set-default "$id" "$W/top"

export BOOT_TOP="$W/top" NEWROOT="$W/root" ROOTDEV
ROOTDEV=$(findmnt -nro SOURCE "$W/top" | cut -d '[' -f1)
export ROOTFLAGS= KERNEL=test-kernel INIT=/usr/lib/systemd/systemd
export BOOT_POLICY="$REPO/initramfs/boot-root.sh"
shell=(bash)
if [ $# -gt 0 ]; then shell=(qemu-aarch64-static -L "$1" "$1/usr/bin/busybox" sh); fi
"${shell[@]}" <<'TEST'
set -eu
. "$BOOT_POLICY"
msg() { echo "test-init: $*"; }
# Initial mount follows the default, not a hard-coded @ path.
prepare_default_boot
test "$(cat "$BOOT_TOP/q25-boot/boot-count")" = 1
umount "$NEWROOT"
prepare_default_boot
test "$(cat "$BOOT_TOP/q25-boot/boot-count")" = 2
umount "$NEWROOT"
prepare_default_boot
test "$(cat "$BOOT_TOP/q25-boot/boot-count")" = 3
umount "$NEWROOT"
# Fourth attempt recovers newest compatible, boot-verified snapshot.
prepare_default_boot
test "$(cat "$BOOT_TOP/q25-boot/recovery-attempt")" = 20
test "$(cat "$BOOT_TOP/q25-boot/boot-count")" = 1
btrfs subvolume get-default "$BOOT_TOP" | grep -q 'q25-boot/recovery-20-'
touch "$NEWROOT/recovery-is-writable"
test ! -e "$BOOT_TOP/@snapshots/20/snapshot/recovery-is-writable"
umount "$NEWROOT"
# Two more failures allowed, but never a second automatic recovery loop.
prepare_default_boot
umount "$NEWROOT"
prepare_default_boot
umount "$NEWROOT"
if prepare_default_boot; then echo 'FAIL: repeated recovery'; exit 1; fi
# Manual snapper rollback/default selection still goes through the modules guard.
rm "$BOOT_TOP/q25-boot/boot-count" "$BOOT_TOP/q25-boot/recovery-attempt"
btrfs property set "$BOOT_TOP/@snapshots/30/snapshot" ro false
id=$(btrfs inspect-internal rootid "$BOOT_TOP/@snapshots/30/snapshot")
btrfs subvolume set-default "$id" "$BOOT_TOP"
if prepare_default_boot; then echo 'FAIL: missing kernel modules accepted'; exit 1; fi
if mountpoint -q "$NEWROOT"; then echo 'FAIL: unsafe root left mounted'; exit 1; fi
# No usable checkpoint is a rescue condition, not an unverified rollback.
rm -f "$BOOT_TOP/q25-boot/good/"*
echo 3 > "$BOOT_TOP/q25-boot/boot-count"
if prepare_default_boot; then echo 'FAIL: unverified recovery'; exit 1; fi
# Corrupt counter must not cause arithmetic errors or block a repaired default.
id=$(btrfs inspect-internal rootid "$BOOT_TOP/@")
btrfs subvolume set-default "$id" "$BOOT_TOP"
echo corrupt > "$BOOT_TOP/q25-boot/boot-count"
prepare_default_boot
test "$(cat "$BOOT_TOP/q25-boot/boot-count")" = 1
umount "$NEWROOT"
echo 'PASS: default mount, counter, newest compatible fallback, writable clone, one-shot recovery, modules guard, corrupt state'
TEST
