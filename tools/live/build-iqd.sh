#!/bin/sh
# SPDX-License-Identifier: MIT
# Cross-compile the iqd capture daemon for the OpenWrt One (aarch64 musl) and,
# with a host argument, push it to the router.
#
#   ./build-iqd.sh                 # build only -> <scratch>/iqd
#   ./build-iqd.sh 192.168.68.70   # build and scp-less copy to root@HOST:/root/iqd
set -e
REPO=$(cd "$(dirname "$0")/../.." && pwd)
TC="$REPO/build/openwrt/staging_dir/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl"
export STAGING_DIR="$REPO/build/openwrt/staging_dir"
OUT=${IQD_OUT:-/tmp/iqd}
"$TC/bin/aarch64-openwrt-linux-musl-gcc" -O2 -static -o "$OUT" "$REPO/tools/live/iqd.c"
echo "built $OUT ($(wc -c < "$OUT") bytes)"
if [ -n "$1" ]; then
	KEY=${OWRT_KEY:-~/.ssh/owrt_one}
	cat "$OUT" | ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
		"root@$1" 'cat > /root/iqd; chmod +x /root/iqd; echo installed /root/iqd'
fi
