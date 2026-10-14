#!/bin/sh
# SPDX-License-Identifier: MIT
# openwrt-one-sdr: gapped native-capture loop, run ON the router.
# stdout: repeated frames of 'IQ01' + the 65536-byte native window (0x00400000).
# Usage: sh iqloop.sh [frames (0=forever)]
# Needs the diagnostic mt7915e (icap_prime + icap_window) and radios up.
. /tmp/icap-device.sh >/dev/null || exit 1
W=$D/icap_window
N=${1:-0}
i=0
# Only one loop may drive the capture engine. dropbear children ignore SIGPIPE,
# so an orphaned loop would otherwise keep running after its SSH pipe closes.
PIDF=/tmp/iqloop.pid
[ -f "$PIDF" ] && kill "$(cat "$PIDF")" 2>/dev/null
echo $$ > "$PIDF"
while [ "$N" = 0 ] || [ "$i" -lt "$N" ]; do
	native_capture
	t=0                                     # wait for completion: CR0 0x401
	while [ "$(rr 0x80023090)" != 0x00000401 ] && [ "$t" -lt 500 ]; do t=$((t+1)); done
	kb 0; kb 0x0200000000000058; kb 0x1400000000000000   # retire: func0, enable0
	wr 0x80023090 0x400                     # clear ICAP_EN before reading
	printf 'IQ01' || break                  # reader gone (EPIPE): stop
	cat "$W" || break
	i=$((i+1))
done
[ "$(cat "$PIDF" 2>/dev/null)" = "$$" ] && rm -f "$PIDF"
