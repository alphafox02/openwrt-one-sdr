#!/bin/sh
# SPDX-License-Identifier: MIT
# Deploy the SDR driver, firmware and control scripts to an OpenWrt One that is
# already running the OpenWrt image from docs/reproduce.md.
#
# Stages a guarded boot pair; does not reload the running radio. See init/README.md.
#
#   init/deploy.sh [BOARD_IP] [SSH_KEY]
set -eu
BOARD=${1:-10.77.0.10}
KEY=${2:-$HOME/.ssh/owrt_one}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ssh_board() { ssh -i "$KEY" -o StrictHostKeyChecking=accept-new "root@$BOARD" "$@"; }
copy_board() { scp -O -i "$KEY" -o StrictHostKeyChecking=accept-new "$@"; }
KO="$ROOT/build/openwrt/build_dir/target-aarch64_cortex-a53_musl/linux-mediatek_filogic/mt76-2024.10.28~c246fa54/mt7915/mt7915e.ko"
TC="$ROOT/build/openwrt/staging_dir/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl/bin/aarch64-openwrt-linux-musl-gcc"
export STAGING_DIR="$ROOT/build/openwrt/staging_dir"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

# Fail before touching the board if a complete driver rebuild fails.
python3 "$ROOT/kernel/mt7915-icap/rebuild.py"
sha256sum "$KO"

echo "1/6 build the func6 / dedicated-pool firmware"
python3 "$ROOT/firmware/streaming/build.py" "$TMP/fw" --pool >/dev/null

echo "2/6 cross-compile the on-device tools"
"$TC" -O2 -o "$TMP/iq-c2d"         "$ROOT/tools/live/iq-c2d.c"
"$TC" -O2 -o "$TMP/iq-sched"       "$ROOT/tools/live/iq-sched.c"
"$TC" -O2 -o "$TMP/vita49-control" "$ROOT/host/controller/vita49-control.c"

echo "3/6 copy firmware, driver, scripts and tools to the board"
ssh_board 'set -e
    test -s /root/wm.gold
    test -s /root/mt7915e.stock.ko
    /etc/init.d/fwguard enabled
    /etc/init.d/sdrdriverguard enabled
    mkdir -p /root/firmware /root/.sdr-stage
    # Keep a partial upload from being selected after an interrupted deployment.
    if [ -x /etc/init.d/owrt-sdr-prepare ]; then /etc/init.d/owrt-sdr-prepare disable; fi
    if [ -x /etc/init.d/owrt-sdr ]; then /etc/init.d/owrt-sdr disable; fi'
# Stage firmware in overlay. Selection happens before kmodloader on reboot.
copy_board "$TMP/fw/wm-window-dma.bin" "root@$BOARD:/root/.sdr-stage/mt7981_wm.sdr.bin"
copy_board "$KO" "root@$BOARD:/root/.sdr-stage/mt7915e.vrt.ko"
copy_board "$ROOT/tools/icap-device.sh" "$ROOT/tools/live/iq-udp-start.sh" \
     "$ROOT/tools/live/iq-udp-stop.sh" "$ROOT/tools/live/iq-radio.sh" "$TMP/iq-c2d" "$TMP/iq-sched" \
     "$TMP/vita49-control" "$ROOT/host/controller/vita49-apply.sh" "root@$BOARD:/root/.sdr-stage/"
copy_board "$ROOT/init/owrt-sdr.init" "root@$BOARD:/root/.sdr-stage/owrt-sdr"
copy_board "$ROOT/init/owrt-sdr-prepare.init" "root@$BOARD:/root/.sdr-stage/owrt-sdr-prepare"

echo "4/6 verify uploaded firmware and driver"
python3 - "$TMP/fw/wm-window-dma.bin" "$KO" > "$TMP/sdr.sha256" <<'PYHASH'
import hashlib, sys
for src, dst in zip(sys.argv[1:], ('/root/.sdr-stage/mt7981_wm.sdr.bin', '/root/.sdr-stage/mt7915e.vrt.ko')):
    with open(src, 'rb') as f:
        print(hashlib.sha256(f.read()).hexdigest(), dst)
PYHASH
copy_board "$TMP/sdr.sha256" "root@$BOARD:/root/firmware/sdr.sha256"
ssh_board 'set -e
    sha256sum -c /root/firmware/sdr.sha256
    # Rename new inodes over executables: overwriting a running binary fails
    # with ETXTBSY. Boot selection remains disabled until all installs succeed.
    mv /root/.sdr-stage/mt7981_wm.sdr.bin /root/firmware/mt7981_wm.sdr.bin
    for f in mt7915e.vrt.ko icap-device.sh iq-udp-start.sh iq-udp-stop.sh iq-radio.sh iq-c2d iq-sched vita49-control vita49-apply.sh; do
        mv "/root/.sdr-stage/$f" "/root/$f"
    done
    mv /root/.sdr-stage/owrt-sdr /etc/init.d/owrt-sdr
    mv /root/.sdr-stage/owrt-sdr-prepare /etc/init.d/owrt-sdr-prepare
    sha256sum /root/firmware/mt7981_wm.sdr.bin /root/mt7915e.vrt.ko > /root/firmware/sdr.sha256
    chmod +x /root/iq-udp-start.sh /root/iq-udp-stop.sh /root/icap-device.sh \
        /root/iq-radio.sh /root/iq-c2d /root/iq-sched /root/vita49-control /root/vita49-apply.sh \
        /etc/init.d/owrt-sdr /etc/init.d/owrt-sdr-prepare'

echo "5/6 allow inbound control on the 2.5 GbE link (udp/4992 C2, udp/4991 VITA 49.2)"
# The IQ stream leaves the board outbound and needs no rule; the control paths are
# inbound, so the firewall must accept them on the zone the link is in (wan here,
# as the board is a LAN-attached SDR through its 2.5 GbE wan port). udp/4992 is the
# viewer's C2 daemon, udp/4991 the VITA 49.2 responder.
ssh_board 'set -e
      for spec in "Allow-IQ-C2 4992" "Allow-VITA49-Control 4991"; do
        nm=${spec% *}; port=${spec#* }
        if ! uci show firewall | grep -q "name=.$nm."; then
          r=$(uci add firewall rule)
          uci set firewall.$r.name="$nm"
          uci set firewall.$r.src="wan"
          uci set firewall.$r.proto="udp"
          uci set firewall.$r.src_ip="10.77.0.11"
          uci set firewall.$r.dest_ip="10.77.0.10"
          uci set firewall.$r.dest_port="$port"
          uci set firewall.$r.target="ACCEPT"
          echo "  added $nm (udp/$port)"
        else echo "  $nm already present"; fi
      done
      uci commit firewall
      /etc/init.d/firewall reload >/dev/null 2>&1'

echo "6/6 enable guarded boot selection (active radio unchanged)"
ssh_board '/etc/init.d/owrt-sdr-prepare enable && /etc/init.d/owrt-sdr enable'
echo "Files staged and boot services enabled. Reboot to apply; no live driver reload was attempted."
echo "If recovery is latched, inspect the failure, then run /etc/init.d/owrt-sdr-prepare rearm before reboot."
