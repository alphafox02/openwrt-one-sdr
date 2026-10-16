#!/bin/sh
# SPDX-License-Identifier: MIT
# Assemble the release assets from a completed source build. Produces, in OUT:
#   openwrt-one-sdr-<ver>-squashfs-sysupgrade.itb   (stock fw + this project's driver)
#   openwrt-one-sdr-<ver>-sdr-tools.tar.gz          (host bring-up bundle, no toolchains)
#   SHA256SUMS
#
#   release/pack.sh [VERSION] [OUT_DIR]
#
# The image and compiled binaries are build outputs; this is how they are
# distributed. No MediaTek firmware is packaged — see release/README.md.
set -eu

VER=${1:-v0.1.0}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${2:-$ROOT/build/release}
B=$ROOT/build/openwrt
TC=$B/staging_dir/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl/bin/aarch64-openwrt-linux-musl-gcc
export STAGING_DIR=$B/staging_dir

IMG=$B/bin/targets/mediatek/filogic/openwrt-mediatek-filogic-openwrt_one-squashfs-sysupgrade.itb
VRT_KO=$B/build_dir/target-aarch64_cortex-a53_musl/linux-mediatek_filogic/mt76-2024.10.28~c246fa54/mt7915/mt7915e.ko
STOCK_KO=$B/build_dir/target-aarch64_cortex-a53_musl/root.orig-mediatek/lib/modules/6.6.63/mt7915e.ko

for f in "$IMG" "$VRT_KO" "$STOCK_KO" "$TC"; do
    [ -e "$f" ] || { echo "missing build output: $f" >&2; echo "run the source build first (docs/reproduce.md)" >&2; exit 1; }
done

rm -rf "$OUT"; mkdir -p "$OUT"
BUNDLE=$OUT/openwrt-one-sdr-$VER-sdr-tools
mkdir -p "$BUNDLE/artifacts" "$BUNDLE/scripts" "$BUNDLE/init"

echo "image"
cp "$IMG" "$OUT/openwrt-one-sdr-$VER-squashfs-sysupgrade.itb"

echo "host bring-up + firmware patcher"
cp "$ROOT/init/setup.sh"                       "$BUNDLE/setup.sh"
cp "$ROOT/firmware/streaming/apply-producer.py" "$BUNDLE/apply-producer.py"
cp "$ROOT/tools/wm_regions.py"                  "$BUNDLE/wm_regions.py"

echo "drivers"
cp "$VRT_KO"   "$BUNDLE/artifacts/mt7915e.vrt.ko"
cp "$STOCK_KO" "$BUNDLE/artifacts/mt7915e.stock.ko"

echo "cross-compile on-device tools"
"$TC" -O2 -o "$BUNDLE/artifacts/iq-c2d"         "$ROOT/tools/live/iq-c2d.c"
"$TC" -O2 -o "$BUNDLE/artifacts/iq-sched"       "$ROOT/tools/live/iq-sched.c"
"$TC" -O2 -o "$BUNDLE/artifacts/vita49-control" "$ROOT/host/controller/vita49-control.c"

echo "scripts"
cp "$ROOT/tools/icap-device.sh" "$ROOT/tools/live/iq-udp-start.sh" \
   "$ROOT/tools/live/iq-udp-stop.sh" "$ROOT/tools/live/iq-radio.sh" \
   "$ROOT/host/controller/vita49-apply.sh" "$BUNDLE/scripts/"

echo "boot guards and services"
cp "$ROOT/init/owrt-sdr.init"           "$BUNDLE/init/owrt-sdr"
cp "$ROOT/init/owrt-sdr-prepare.init"   "$BUNDLE/init/owrt-sdr-prepare"
cp "$ROOT/init/host-link.sh"            "$BUNDLE/init/host-link.sh"
cp "$ROOT/platform/fwguard/fwguard.init" "$BUNDLE/init/fwguard"
cp "$ROOT/kernel/mt7915-icap/sdrdriverguard" "$BUNDLE/init/sdrdriverguard"

chmod +x "$BUNDLE/setup.sh" "$BUNDLE/apply-producer.py" \
    "$BUNDLE/scripts/"*.sh "$BUNDLE/init/"*

echo "tarball"
( cd "$OUT" && tar czf "openwrt-one-sdr-$VER-sdr-tools.tar.gz" "openwrt-one-sdr-$VER-sdr-tools" )
rm -rf "$BUNDLE"

echo "checksums"
( cd "$OUT" && sha256sum openwrt-one-sdr-$VER-*.itb openwrt-one-sdr-$VER-*.tar.gz > SHA256SUMS )

echo "done -> $OUT"
ls -la "$OUT"
