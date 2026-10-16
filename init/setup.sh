#!/bin/sh
# SPDX-License-Identifier: MIT
# One-command SDR bring-up for a freshly flashed OpenWrt One, from the release
# bundle (no build tree, no toolchains).
#
#   setup.sh BOARD_IP [SSH_KEY]
#
# It performs the same board-side steps that init/deploy.sh does on a developer
# board, but sources pre-built artifacts from this bundle and patches the board's
# own stock firmware locally with apply-producer.py (no nds32 toolchain). It ships
# and transmits no MediaTek firmware: the SDR firmware is derived on your machine
# from the stock blob already on your board, and is never redistributed.
#
# Expected starting point: an OpenWrt One running the release image (or stock
# OpenWrt at the pinned version), reachable over SSH on its LAN. See
# release/README.md and the project README "Getting started".
set -eu

BOARD=${1:?usage: setup.sh BOARD_IP [SSH_KEY]}
KEY=${2:-$HOME/.ssh/owrt_one}
HERE=$(cd "$(dirname "$0")" && pwd)
STOCK_MD5=e11d45ca356de61d17b1024097ef8137

ssh_board() { ssh -i "$KEY" -o StrictHostKeyChecking=accept-new "root@$BOARD" "$@"; }
copy_board() { scp -O -i "$KEY" -o StrictHostKeyChecking=accept-new "$@"; }

# Resolve bundle files whether this runs from the release tarball (flat) or from
# the repo checkout (artifacts built by deploy.sh / the release packer).
find_file() {
    for p in "$HERE/$1" "$HERE/artifacts/$1" "$HERE/scripts/$1" "$HERE/init/$1"; do
        [ -f "$p" ] && { echo "$p"; return; }
    done
    echo "missing bundle file: $1" >&2; exit 1
}

VRT_KO=$(find_file mt7915e.vrt.ko)
STOCK_KO=$(find_file mt7915e.stock.ko)
APPLY=$(find_file apply-producer.py)
IQ_C2D=$(find_file iq-c2d)
IQ_SCHED=$(find_file iq-sched)
VITA_CTRL=$(find_file vita49-control)

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

echo "1/7 pull the board's own stock Wi-Fi firmware and patch it locally"
copy_board "root@$BOARD:/lib/firmware/mediatek/mt7981_wm.bin" "$TMP/stock_wm.bin"
got=$(md5sum "$TMP/stock_wm.bin" | cut -d' ' -f1)
if [ "$got" != "$STOCK_MD5" ]; then
    echo "  refusing: board firmware md5 $got is not the pinned stock blob ($STOCK_MD5)." >&2
    echo "  Flash the release image (or stock OpenWrt at the pinned version) first." >&2
    exit 1
fi
PYTHONPATH="$HERE" python3 "$APPLY" "$TMP/stock_wm.bin" "$TMP/mt7981_wm.sdr.bin"

echo "2/7 preflight the board (must have gold backups creatable from its stock files)"
ssh_board 'set -e
    test -f /lib/firmware/mediatek/mt7981_wm.bin
    test -f /lib/firmware/mediatek/mt7981_wa.bin
    test -f /lib/firmware/mediatek/mt7981_rom_patch.bin
    mkdir -p /root/firmware /root/.sdr-stage
    # A partial upload must never be selected after an interrupted run.
    for s in owrt-sdr-prepare owrt-sdr; do
        [ -x "/etc/init.d/$s" ] && /etc/init.d/$s disable || true
    done'

echo "3/7 stage firmware, drivers, tools, scripts, guards and services"
copy_board "$TMP/mt7981_wm.sdr.bin" "root@$BOARD:/root/.sdr-stage/mt7981_wm.sdr.bin"
copy_board "$VRT_KO"   "root@$BOARD:/root/.sdr-stage/mt7915e.vrt.ko"
copy_board "$STOCK_KO" "root@$BOARD:/root/.sdr-stage/mt7915e.stock.ko"
copy_board "$IQ_C2D" "$IQ_SCHED" "$VITA_CTRL" "root@$BOARD:/root/.sdr-stage/"
copy_board "$(find_file icap-device.sh)" "$(find_file iq-udp-start.sh)" \
    "$(find_file iq-udp-stop.sh)" "$(find_file iq-radio.sh)" \
    "$(find_file vita49-apply.sh)" "root@$BOARD:/root/.sdr-stage/"
copy_board "$(find_file owrt-sdr)"         "root@$BOARD:/root/.sdr-stage/owrt-sdr"
copy_board "$(find_file owrt-sdr-prepare)" "root@$BOARD:/root/.sdr-stage/owrt-sdr-prepare"
copy_board "$(find_file fwguard)"          "root@$BOARD:/root/.sdr-stage/fwguard"
copy_board "$(find_file sdrdriverguard)"   "root@$BOARD:/root/.sdr-stage/sdrdriverguard"

echo "4/7 verify the staged SDR firmware and driver by hash"
python3 - "$TMP/mt7981_wm.sdr.bin" "$VRT_KO" "$STOCK_KO" > "$TMP/sdr.sha256" <<'PYHASH'
import hashlib, sys
dsts = ('/root/.sdr-stage/mt7981_wm.sdr.bin',
        '/root/.sdr-stage/mt7915e.vrt.ko',
        '/root/.sdr-stage/mt7915e.stock.ko')
for src, dst in zip(sys.argv[1:], dsts):
    with open(src, 'rb') as f:
        print(hashlib.sha256(f.read()).hexdigest(), dst)
PYHASH
copy_board "$TMP/sdr.sha256" "root@$BOARD:/root/firmware/sdr.stage.sha256"
ssh_board 'sha256sum -c /root/firmware/sdr.stage.sha256'

echo "5/7 establish gold backups from the board's own stock files (kept if present)"
ssh_board 'set -e
    cd /lib/firmware/mediatek
    [ -s /root/wm.gold ]        || cp mt7981_wm.bin        /root/wm.gold
    [ -s /root/wa.gold ]        || cp mt7981_wa.bin        /root/wa.gold
    [ -s /root/rom_patch.gold ] || cp mt7981_rom_patch.bin /root/rom_patch.gold
    [ -s /root/mt7915e.stock.ko ] || mv /root/.sdr-stage/mt7915e.stock.ko /root/mt7915e.stock.ko
    rm -f /root/.sdr-stage/mt7915e.stock.ko 2>/dev/null || true'

echo "6/7 install SDR firmware, driver, tools, scripts, guards and services"
ssh_board 'set -e
    mv /root/.sdr-stage/mt7981_wm.sdr.bin /root/firmware/mt7981_wm.sdr.bin
    mv /root/.sdr-stage/mt7915e.vrt.ko /root/mt7915e.vrt.ko
    for f in icap-device.sh iq-udp-start.sh iq-udp-stop.sh iq-radio.sh \
             iq-c2d iq-sched vita49-control vita49-apply.sh; do
        mv "/root/.sdr-stage/$f" "/root/$f"
    done
    mv /root/.sdr-stage/owrt-sdr         /etc/init.d/owrt-sdr
    mv /root/.sdr-stage/owrt-sdr-prepare /etc/init.d/owrt-sdr-prepare
    mv /root/.sdr-stage/fwguard          /etc/init.d/fwguard
    mv /root/.sdr-stage/sdrdriverguard   /etc/init.d/sdrdriverguard
    chmod +x /root/iq-udp-start.sh /root/iq-udp-stop.sh /root/icap-device.sh \
        /root/iq-radio.sh /root/iq-c2d /root/iq-sched /root/vita49-control \
        /root/vita49-apply.sh /etc/init.d/owrt-sdr /etc/init.d/owrt-sdr-prepare \
        /etc/init.d/fwguard /etc/init.d/sdrdriverguard
    sha256sum /root/firmware/mt7981_wm.sdr.bin /root/mt7915e.vrt.ko > /root/firmware/sdr.sha256
    rmdir /root/.sdr-stage 2>/dev/null || true'

echo "7/7 open inbound control on the 2.5 GbE link and enable guarded boot"
# The IQ stream leaves the board outbound and needs no rule. Two control paths are
# inbound and the wan zone rejects by default, so both ports must be accepted:
# udp/4992 is the viewer's C2 daemon, udp/4991 the VITA 49.2 responder.
ssh_board 'set -e
    for spec in "Allow-IQ-C2 4992" "Allow-VITA49-Control 4991"; do
        nm=${spec% *}; port=${spec#* }
        if ! uci show firewall | grep -q "name=.$nm."; then
            r=$(uci add firewall rule)
            uci set firewall.$r.name="$nm"
            uci set firewall.$r.src="wan"; uci set firewall.$r.proto="udp"
            uci set firewall.$r.src_ip="10.77.0.11"; uci set firewall.$r.dest_ip="10.77.0.10"
            uci set firewall.$r.dest_port="$port"; uci set firewall.$r.target="ACCEPT"
        fi
    done
    uci commit firewall; /etc/init.d/firewall reload >/dev/null 2>&1
    # Start with a 40 MHz receive-only block centred at 2437 MHz. Monitor mode
    # avoids AP coexistence reducing the requested 40 MHz width to 20 MHz.
    uci set wireless.radio0.disabled=0
    uci set wireless.radio1.disabled=1
    uci set wireless.radio0.channel=4
    uci set wireless.radio0.htmode=HE40
    uci set wireless.default_radio0.mode=monitor
    uci -q delete wireless.default_radio0.network || true
    uci commit wireless
    /etc/init.d/fwguard enable
    /etc/init.d/sdrdriverguard enable
    /etc/init.d/owrt-sdr-prepare enable
    /etc/init.d/owrt-sdr enable'

cat <<EOF
Done. The board is staged; nothing on the running radio was touched.

Reboot the board to apply:   ssh -i $KEY root@$BOARD reboot

After it comes back on the 2.5 GbE link (board 10.77.0.10), on this host:
  init/host-link.sh <your-2.5GbE-iface>           # 10.77.0.11/24 on the cable
  ./vita49-shm --port 4991 --stream /w1700k-stream &
  host/spectrum/build/w1700k-spectrum --center 2437 --rate 40 \\
      --stream /w1700k-stream --listen 127.0.0.1:8073
Then open http://127.0.0.1:8073

Recovery: an unclean power-off latches the next boot back to stock; after
inspecting, run  /etc/init.d/owrt-sdr-prepare rearm  before rebooting. See
init/README.md.
EOF
