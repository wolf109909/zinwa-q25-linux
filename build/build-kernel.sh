#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the Q25 kernel (Image.gz with embedded DTB), in-tree modules and all
# out-of-tree Q25 drivers, staged into $KSTAGE/lib/modules/<release>.
#
#   build/build-kernel.sh
#
# Environment:
#   KSRC    patched Linux 7.3.3 tree   (default ~/q25-work/linux-7.3)
#   KSTAGE  staging output             (default ~/q25-work/kstage)
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
KSRC=${KSRC:-$HOME/q25-work/linux-7.3}
KSTAGE=${KSTAGE:-$HOME/q25-work/kstage}
MK=(make -C "$KSRC" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- LOCALVERSION= -j"$(nproc)")
DTS=arch/arm64/boot/dts/mediatek/mt8781-zinwa-q25.dts

log() { printf '\033[1;32m==> %s\033[0m\n' "$*"; }

log "sync kernel config"
CFG="$REPO/cachyos/config-mt6789-mainline-7.3.aarch64"
if ! cmp -s "$CFG" "$KSRC/.config"; then
    cp "$CFG" "$KSRC/.config"
    "${MK[@]}" olddefconfig
fi

log "sync device tree"
if ! cmp -s "$REPO/devicetree/mt8781-zinwa-q25.dts" "$KSRC/$DTS"; then
    cp "$REPO/devicetree/mt8781-zinwa-q25.dts" "$KSRC/$DTS"
fi
"${MK[@]}" mediatek/mt8781-zinwa-q25.dtb
# head.S .incbin's the DTB; make sure it is re-assembled when the DTB changes
if [ "$KSRC/arch/arm64/boot/dts/mediatek/mt8781-zinwa-q25.dtb" -nt "$KSRC/arch/arm64/kernel/head.o" ]; then
    touch "$KSRC/arch/arm64/kernel/head.S"
fi

log "kernel Image.gz + modules"
"${MK[@]}" Image.gz modules
KREL=$(cat "$KSRC/include/config/kernel.release")

log "stage in-tree modules for $KREL"
rm -rf "$KSTAGE"
mkdir -p "$KSTAGE/boot"
"${MK[@]}" INSTALL_MOD_PATH="$KSTAGE" INSTALL_MOD_STRIP=1 modules_install >/dev/null
cp "$KSRC/arch/arm64/boot/Image.gz" "$KSTAGE/boot/Image.gz"
cp "$KSRC/arch/arm64/boot/dts/mediatek/mt8781-zinwa-q25.dtb" "$KSTAGE/boot/"
cp "$KSRC/.config" "$KSTAGE/boot/config-$KREL"
cp "$KSRC/System.map" "$KSTAGE/boot/System.map-$KREL"

log "out-of-tree Q25 drivers"
DEST="$KSTAGE/lib/modules/$KREL/updates/q25"
mkdir -p "$DEST"
for d in "$REPO/drivers" "$REPO/drivers/mediatek"; do
    "${MK[@]}" M="$d" modules
done
# collect every .ko built under drivers/ (one per module name)
find "$REPO/drivers" -name '*.ko' | while read -r ko; do
    install -m644 "$ko" "$DEST/"
done
aarch64-linux-gnu-strip --strip-debug "$DEST"/*.ko
depmod -b "$KSTAGE" "$KREL"
echo "$KREL" > "$KSTAGE/kernel.release"

log "done: $KREL ($(ls "$DEST" | wc -l) Q25 modules, $(du -sh "$KSTAGE/lib/modules" | cut -f1))"
