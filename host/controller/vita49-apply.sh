#!/bin/sh
# SPDX-License-Identifier: MIT
# Ordered control hook; readback describes the active radio and next capture.
set -eu
RADIO=${IQ_RADIO_HELPER:-/root/iq-radio.sh}
[ "${1:-}" != --state ] || exec "$RADIO" --state
check=0
if [ "${1:-}" = --check ]; then check=1; shift; fi
[ "$#" -eq 2 ] || exit 2
case "$1" in
rf_freq_hz)
    "$RADIO" --check-frequency "$2"
    [ "$check" -eq 1 ] || "$RADIO" tune "$2"
    ;;
sample_rate_hz)
    "$RADIO" --check-rate "$2"
    [ "$check" -eq 1 ] || "$RADIO" rate "$2"
    ;;
bandwidth_hz)
    # RF filter changes are not implemented; only acknowledge an unchanged width.
    [ "$2" = 20000000 ] || exit 1
    "$RADIO" --state >/dev/null
    ;;
*) echo "unsupported control: $1" >&2; exit 1;;
esac
