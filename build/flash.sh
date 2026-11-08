#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# Flash a built Q25 image from a Linux host with the phone in fastboot mode
# (power + volume-up, or `reboot bootloader` from Linux).
#
#   build/flash.sh [outdir]          default outdir: $Q25_OUT or <repo>/out
#
# WIPES userdata (all Android/Linux user content). Writes boot to BOTH slots so
# an A/B fallback never lands in an old kernel. Nothing else is touched:
# vendor_boot, vbmeta, nvdata/nvram (IMEI, Wi-Fi/BT calibration) stay stock.
# Windows: run the same `fastboot` commands with fastboot.exe; the userdata
# chunks are small on purpose (32-bit fastboot.exe cannot load big images).
set -euo pipefail
OUT=${1:-${Q25_OUT:-$(cd "$(dirname "$0")/.." && pwd)/out}}
FB=${FASTBOOT:-fastboot}

[ -f "$OUT/boot.img" ] && [ -d "$OUT/userdata" ] ||
    { echo "missing $OUT/boot.img or $OUT/userdata/ (run build/build-all.sh)" >&2; exit 1; }

$FB devices | grep -q fastboot || { echo "no device in fastboot mode" >&2; exit 1; }
product=$($FB getvar product 2>&1 | sed -n 's/^product: *//p')
echo "device product: ${product:-unknown}"
read -r -p "This ERASES userdata on the phone. Type YES to continue: " ok
[ "$ok" = YES ] || exit 1

$FB flash boot_a "$OUT/boot.img"
$FB flash boot_b "$OUT/boot.img"
$FB erase userdata || true
for c in "$OUT"/userdata/userdata_*.simg; do
    $FB flash userdata "$c"
done
$FB set_active a
$FB reboot
echo "Flashed. First boot grows the root filesystem to the whole userdata partition."
