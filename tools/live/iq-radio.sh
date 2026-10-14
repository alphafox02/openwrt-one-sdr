#!/bin/sh
# SPDX-License-Identifier: MIT
# One active radio, HE20 RF filter. Capture divider is separate from RF width.
# Profiles measured on this MT7981: 2g=40 MSps; 5g=40 MSps (80 experimental).
set -eu
profile() {
    if [ "$(uci -q get wireless.radio0.disabled || echo 0)" != 1 ]; then
        [ "$(uci -q get wireless.radio1.disabled || echo 0)" = 1 ] || return 1
        band=0; radio=radio0; rate=40000000; bw=0
    else
        [ "$(uci -q get wireless.radio1.disabled || echo 0)" != 1 ] || return 1
        band=1; radio=radio1
        rate=$(uci -q get wireless.radio1.sdr_sample_rate || echo 40000000)
        case "$rate" in 40000000) bw=0;; *) return 1;; esac
    fi
    channel=$(uci get "wireless.$radio.channel")
    [ "$(uci get "wireless.$radio.htmode")" = HE20 ] || return 1
    if [ "$band" = 0 ]; then frequency=$((2407000000+channel*5000000))
    else frequency=$((5000000000+channel*5000000)); fi
    # Read the actual operating channel, not just the requested UCI value.
    iw dev | awk -v wanted="$frequency" '
        $1 == "channel" { gsub(/[()]/,"",$3); if ($3*1000000 == wanted && $0 ~ /width: 20 MHz/) ok=1 }
        END { exit !ok }' || return 1
}
validate_frequency() {
    value=$1
    case "$value" in ''|*[!0-9]*) return 1;; esac
    [ "${#value}" -eq 10 ] || return 1
    if [ "$value" -ge 2412000000 ] && [ "$value" -le 2472000000 ] &&
       [ "$(((value-2412000000)%5000000))" -eq 0 ]; then
        target=radio0; other=radio1; channel=$(((value-2407000000)/5000000))
    else
        case "$value" in 5180000000|5200000000|5220000000|5240000000) ;;
            *) echo 'Supported centres: 2.4 GHz channels 1..13; 5 GHz channels 36/40/44/48' >&2; return 1;; esac
        target=radio1; other=radio0; channel=$(((value-5000000000)/5000000))
    fi
}
case "${1:-}" in
--state|--profile)
    profile
    if [ "$1" = --state ]; then printf '%s %s 20000000\n' "$frequency" "$rate"
    else printf '%s %s 20000000 %s %s\n' "$frequency" "$rate" "$band" "$bw"; fi
    ;;
--check-frequency) [ "$#" -eq 2 ] && validate_frequency "$2";;
tune)
    [ "$#" -eq 2 ]; validate_frequency "$2"
    # A repeated request for the verified active centre must not interrupt a
    # stream or incur another Wi-Fi reload/settle cycle.
    current=$("$0" --state) || current=
    [ "${current%% *}" != "$2" ] || exit 0
    /root/iq-udp-stop.sh
    uci set "wireless.$target.disabled=0"
    uci set "wireless.$other.disabled=1"
    uci set "wireless.$target.channel=$channel"
    uci set "wireless.$target.htmode=HE20"
    uci commit wireless
    wifi reload
    sleep 25
    profile
    [ "$frequency" = "$2" ]
    ;;
--check-rate|rate)
    [ "$#" -eq 2 ]; requested=$2; profile
    case "$band:$requested" in 0:40000000|1:40000000) ;; *) exit 1;; esac
    if [ "$1" = rate ]; then
        [ "$requested" != "$rate" ] || exit 0
        /root/iq-udp-stop.sh
        if [ "$band" = 1 ]; then
            uci set "wireless.radio1.sdr_sample_rate=$requested"
            uci commit wireless
        fi
    fi
    ;;
*) echo 'usage: iq-radio.sh --state|--profile|--check-frequency HZ|tune HZ|--check-rate HZ|rate HZ' >&2; exit 2;;
esac
