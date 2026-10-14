#!/bin/sh
# Read only mapped diagnostic mailbox words; run after acquisition finishes.
set -eu
. /root/icap-device.sh
cat "$D/iq_udp"
for off in 0 4 8 24 28 96 100 104 108 112 116 120 124 128 132 136 140 144 148 152; do
    printf '%3s ' "$off"
    rr "$((0xe009b800 + off))"
done
