#!/bin/sh
# SPDX-License-Identifier: MIT
# Verified HE40 profiles. Report the operating block centre, not its primary
# Wi-Fi channel. Capture rate and RF channel width are separate quantities.
set -eu
profile() {
    if [ "$(uci -q get wireless.radio0.disabled || echo 0)" != 1 ]; then
        [ "$(uci -q get wireless.radio1.disabled || echo 0)" = 1 ] || return 1
        band=0; radio=radio0
    else
        [ "$(uci -q get wireless.radio1.disabled || echo 0)" != 1 ] || return 1
        band=1; radio=radio1
    fi
    rate=40000000; bw=0; abw=40000000
    [ "$(uci get "wireless.$radio.htmode")" = HE40 ] || return 1
    channel=$(uci get "wireless.$radio.channel")
    case "$channel" in ''|*[!0-9]*) return 1;; esac
    if [ "$band" = 0 ]; then
        [ "$channel" -ge 1 ] && [ "$channel" -le 13 ] || return 1
        [ "$(uci get wireless.default_radio0.mode)" = monitor ] || return 1
        # netifd uses HT40+ on primary 1..6 and HT40- on 7..13; the 40 MHz block
        # centre is 10 MHz from the primary either way.
        if [ "$channel" -le 6 ]; then centre=$((2407000000+channel*5000000+10000000))
        else centre=$((2407000000+channel*5000000-10000000)); fi
    else
        # Only the lower primary of each U-NII-1 HE40 pair (36->5190, 44->5230).
        case "$channel" in 36|44) ;; *) return 1;; esac
        centre=$((5000000000+channel*5000000+10000000))
    fi
    # Verify against the actual 40 MHz block centre (iw center1) and width, not the
    # primary channel: the driver may realise either primary of the pair, so a
    # primary match is fragile. Also refuse a 20 MHz coexistence fallback or a
    # non-monitor 2.4 GHz interface.
    frequency=$(iw dev | awk -v wanted="$centre" -v band="$band" '
        $1 == "type" { iface_type=$2 }
        $1 == "channel" {
            if (band == 0 && iface_type != "monitor") next
            if ($6 != 40) next
            for (i=1;i<NF;i++) if ($i == "center1:") c=$(i+1)*1000000
            if (c == wanted) matches++
        }
        END { if (matches != 1) exit 1; printf "%.0f", wanted }') || return 1
}
validate_frequency() {
    value=$1
    case "$value" in ''|*[!0-9]*) return 1;; esac
    [ "${#value}" -eq 10 ] || return 1
    if [ "$value" -ge 2422000000 ] && [ "$value" -le 2462000000 ] &&
       [ "$(((value-2422000000)%5000000))" -eq 0 ]; then
        target=radio0; other=radio1
        # netifd selects HT40+ on primary 1..6, HT40- on primary 7..13.
        if [ "$value" -le 2447000000 ]; then channel=$(((value-2417000000)/5000000))
        else channel=$(((value-2397000000)/5000000)); fi
    else
        case "$value" in 5190000000) channel=36;; 5230000000) channel=44;;
            *) echo 'Supported HE40 centres: 2422..2462 MHz in 5 MHz steps; 5190/5230 MHz' >&2; return 1;; esac
        target=radio1; other=radio0
    fi
}
case "${1:-}" in
--state|--profile)
    profile
    if [ "$1" = --state ]; then printf '%s %s %s\n' "$frequency" "$rate" "$abw"
    else printf '%s %s %s %s %s\n' "$frequency" "$rate" "$abw" "$band" "$bw"; fi
    ;;
--check-frequency) [ "$#" -eq 2 ] && validate_frequency "$2";;
tune)
    [ "$#" -eq 2 ]; validate_frequency "$2"
    current=$("$0" --state) || current=
    [ "${current%% *}" != "$2" ] || exit 0
    /root/iq-udp-stop.sh
    uci set "wireless.$target.disabled=0"
    uci set "wireless.$other.disabled=1"
    uci set "wireless.$target.channel=$channel"
    uci set "wireless.$target.htmode=HE40"
    if [ "$target" = radio0 ]; then
        uci set wireless.default_radio0.mode=monitor
        uci -q delete wireless.default_radio0.network || true
    fi
    uci commit wireless
    wifi reload
    sleep 25
    profile
    [ "$frequency" = "$2" ]
    ;;
--check-rate|rate)
    [ "$#" -eq 2 ]; profile
    [ "$2" = "$rate" ]
    ;;
*) echo 'usage: iq-radio.sh --state|--profile|--check-frequency HZ|tune HZ|--check-rate HZ|rate HZ' >&2; exit 2;;
esac
