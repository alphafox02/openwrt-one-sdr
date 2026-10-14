#!/bin/sh
# Source on the router. Select the first PHY with the flex-driver command knob.
D=$(find /sys/kernel/debug/ieee80211 -name icap_prime | head -1)
D=${D%/*}
[ -n "$D" ] || { echo 'ICAP flex driver is not loaded' >&2; return 1; }
rr() { echo "$1" > "$D/regidx"; cat "$D/regval"; }
wr() { echo "$1" > "$D/regidx"; echo "$2" > "$D/regval"; }
kb() { echo "$1" > "$D/icap_prime"; }
sb() { kb "$(printf '0x01000000%04x00%02x' "$1" "$2")"; }
sw() {
 off=$1; val=$2
 sb "$off" "$((val & 255))"
 sb "$((off+1))" "$(((val >> 8) & 255))"
 sb "$((off+2))" "$(((val >> 16) & 255))"
 sb "$((off+3))" "$(((val >> 24) & 255))"
}
ce() { kb 0; kb 0x020000000000000c; sb 0 "$1"; sw 4 "$2"; sw 8 "$3"; kb 0x1300000000000000; sleep 1; }
ext() {
 func=$1; shift
 kb 0; kb 0x0200000000000058; sb 0 1; sw 4 "$func"
 pos=8
 for value in "$@"; do sw "$pos" "$value"; pos=$((pos+4)); done
 kb 0xff00000000000000
 sleep 1
}
# MT7981 native SPECTRUM/ICAP: func@0; enable@4; node@16 (NOT length).
# Normal boot with the radio up. These defaults select band/PHY 0, 14-bit IQ.
native_capture() {
 kb 0; kb 0x0200000000000058
 sw 4 1; sw 8 "${1:-0}"; sw 16 2113536; sw 20 4096; sw 24 4096
 kb 0x1400000000000000
}
native_retire() {
 kb 0; kb 0x0200000000000058; sw 40 "${1:-0}"; sw 56 "${1:-0}"; kb 0x1400000000000000
 sleep 1
 wr 0x80023090 0x400
}
snapshot() {
 for addr in 0x80023090 0x800230b4 0x800230b8 0x830ad448 0x830ad440 0x830a1000 0x88009028 0xe00ff498 0xe009b800 0xe009b804 0xe009b808; do
  printf '%s ' "$addr"; rr "$addr"
 done
}
