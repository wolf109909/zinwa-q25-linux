#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Install a freshly built kernel on a running phone over SSH: the WHOLE module
# tree of that build plus boot_a, together. Never copy single .ko files after a
# config change: without MODVERSIONS a stale in-tree module still loads and then
# uses the old struct layouts (2026-11-22: tun.ko from an older config corrupted
# memory at every shutdown once XFRM had changed struct net).
#
#   build/deploy-kernel.sh [ssh-target]    default root@10.15.19.82 (USB NCM)
#
# Needs build/build-kernel.sh output ($KSTAGE) and $OUT/boot.img
# (build/build-boot.sh). boot_b is left alone as the fallback kernel. The
# previous module tree stays on the phone as /lib/modules/<release>.prev.
# Reboot the phone afterwards.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
KSTAGE=${KSTAGE:-$HOME/q25-work/kstage}
OUT=${OUT:-$REPO/out}
H=${1:-root@10.15.19.82}
KREL=$(cat "$KSTAGE/kernel.release" 2>/dev/null || ls "$KSTAGE/lib/modules")
MD="$KSTAGE/lib/modules/$KREL"

[ -d "$MD/kernel" ] || { echo "no staged modules in $MD (run build/build-kernel.sh)" >&2; exit 1; }
[ -f "$OUT/boot.img" ] || { echo "missing $OUT/boot.img (run build/build-boot.sh)" >&2; exit 1; }
[ "$OUT/boot.img" -nt "$KSTAGE/boot/Image.gz" ] ||
    { echo "$OUT/boot.img is older than the staged kernel: rebuild it" >&2; exit 1; }

echo "==> modules $KREL -> $H"
tar -C "$MD" -cf - --owner=0 --group=0 --exclude='build' --exclude='source' . |
    ssh "$H" "set -e; d=/lib/modules/$KREL; rm -rf \$d.new; mkdir -p \$d.new
              tar -C \$d.new -xf - --no-same-owner
              rm -rf \$d.prev; [ -d \$d ] && mv \$d \$d.prev; mv \$d.new \$d
              depmod -a $KREL"

echo "==> boot_a"
ssh "$H" "cat > /tmp/q25-boot.img" < "$OUT/boot.img"
ssh "$H" "set -e; p=/dev/disk/by-partlabel/boot_a
          [ \$(stat -c %s /tmp/q25-boot.img) -le \$(blockdev --getsize64 \$p) ]
          dd if=/tmp/q25-boot.img of=\$p bs=1M conv=fsync status=none
          cmp -n \$(stat -c %s /tmp/q25-boot.img) /tmp/q25-boot.img \$p && rm /tmp/q25-boot.img"
echo "done: reboot the phone (boot_b keeps the previous fallback kernel)"
