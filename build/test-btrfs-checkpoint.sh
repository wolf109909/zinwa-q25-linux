#!/bin/bash
# Native integration test for q25-boot-ok and real Snapper. No phone/image writes.
# sudo build/test-btrfs-checkpoint.sh  (host btrfs-progs + snapper required)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
[ "$(id -u)" = 0 ] || { echo 'run as root'; exit 1; }
if [ "${1:-}" != --private ]; then
    exec unshare --mount --propagation private "$0" --private
fi
command -v snapper >/dev/null
W=$(mktemp -d)
cleanup() {
    mountpoint -q "$W/root" && umount -R "$W/root"
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
id=$(btrfs inspect-internal rootid "$W/top/@")
btrfs subvolume set-default "$id" "$W/top"
dev=$(findmnt -nro SOURCE "$W/top" | cut -d '[' -f1)
mount "$dev" "$W/root"
R=$W/root
mkdir -p "$R"/{usr/lib/systemd,usr/lib/modules/test-kernel,dev,proc,run,.snapshots,etc/conf.d,etc/snapper/configs,var/log}
printf '#!/bin/sh\n' > "$R/usr/lib/systemd/systemd"
chmod +x "$R/usr/lib/systemd/systemd"
echo test.ko: > "$R/usr/lib/modules/test-kernel/modules.dep"
echo module > "$R/usr/lib/modules/test-kernel/test.ko"
ln -s usr/lib "$R/lib"
ln -s usr/lib "$R/lib64"
ln -s usr/bin "$R/bin"
ln -s /proc/mounts "$R/etc/mtab"
cp "$REPO/rootfs/etc/conf.d/snapper" "$R/etc/conf.d/"
cp "$REPO/rootfs/etc/snapper/configs/root" "$R/etc/snapper/configs/"
cp "$REPO/rootfs/usr/lib/q25/q25-boot-ok" "$R/test-boot-ok"
cp /etc/passwd /etc/group "$R/etc/"
mount --bind /usr "$R/usr"
mount --bind /dev "$R/dev"
mount -t proc proc "$R/proc"
mount -t tmpfs tmpfs "$R/run"
mount -o subvol=@snapshots "$dev" "$R/.snapshots"
mkdir -p "$W/top/q25-boot/good"
echo 3 > "$W/top/q25-boot/boot-count"
echo 1 > "$W/top/q25-boot/recovery-attempt"
chroot "$R" /bin/sh /test-boot-ok
test ! -e "$W/top/q25-boot/boot-count"
test ! -e "$W/top/q25-boot/recovery-attempt"
test "$(ls "$W/top/q25-boot/good" | wc -l)" = 1
first=$(cat "$W/top/q25-boot/last-checkpoint")
chroot "$R" /bin/sh /test-boot-ok
test "$first" = "$(cat "$W/top/q25-boot/last-checkpoint")"
test "$(ls "$W/top/q25-boot/good" | wc -l)" = 1
for n in 1 2 3 4; do
    echo "$n" > "$R/changed"
    chroot "$R" /bin/sh /test-boot-ok
done
test "$(ls "$W/top/q25-boot/good" | wc -l)" = 3
for n in $(ls "$W/top/q25-boot/good"); do
    btrfs property get "$R/.snapshots/$n/snapshot" ro | grep -qx ro=true
done
newest=$(ls "$W/top/q25-boot/good" | sort -rn | head -1)
# Snapper itself must be able to change the default without a bootloader.
chroot "$R" snapper --no-dbus --ambit classic -c root rollback "$newest"
default=$(btrfs subvolume get-default "$W/top")
[[ "$default" == *'path @snapshots/'*'/snapshot' ]]
new_id=$(echo "$default" | awk '{print $2}')
test "$new_id" != "$id"
umount -R "$R"
BOOT_TOP=$W/top NEWROOT=$W/root ROOTDEV=$dev ROOTFLAGS= KERNEL=test-kernel INIT=/usr/lib/systemd/systemd
. "$REPO/initramfs/boot-root.sh"
msg() { echo "test-init: $*"; }
prepare_default_boot
test "$(btrfs inspect-internal rootid "$NEWROOT")" = "$new_id"
test "$(cat "$NEWROOT/changed")" = 4
umount "$NEWROOT"
echo 'PASS: success clears counter, unchanged generation deduplicates, latest 3 good checkpoints retained, initramfs follows Snapper rollback default'
