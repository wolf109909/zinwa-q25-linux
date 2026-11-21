#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Create the patched Linux 7.3.3 source tree used by build-kernel.sh:
# kernel.org tarball + patches/mt6789-soc (SoC + Q25 bring-up) + patches/cachyos-7.3 (BORE).
#
#   build/prepare-kernel.sh            (KSRC default ~/q25-work/linux-7.3)
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
KSRC=${KSRC:-$HOME/q25-work/linux-7.3}
VER=7.3.3
[ -e "$KSRC" ] && { echo "$KSRC exists; remove it or set KSRC" >&2; exit 1; }
mkdir -p "$(dirname "$KSRC")"
cd "$(dirname "$KSRC")"
[ -f "linux-$VER.tar.xz" ] || curl -fLO "https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-$VER.tar.xz"
tar xf "linux-$VER.tar.xz"
[ "linux-$VER" = "$(basename "$KSRC")" ] || mv "linux-$VER" "$KSRC"
cd "$KSRC"
# order matters: SoC series 0001-0023, BORE, then the Q25 7.3 fixes (0024+)
for p in "$REPO"/patches/mt6789-soc/00{01..23}-*.patch \
         "$REPO"/patches/cachyos-7.3/*.patch \
         "$REPO"/patches/mt6789-soc/00{24..99}-*.patch; do
    [ -e "$p" ] || continue
    echo "  -> $(basename "$p")"
    patch -Np1 --quiet -i "$p"
done
cp "$REPO/devicetree/mt8781-zinwa-q25.dts" arch/arm64/boot/dts/mediatek/
cp "$REPO/cachyos/config-mt6789-mainline-7.3.aarch64" .config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
echo "kernel tree ready: $KSRC"
