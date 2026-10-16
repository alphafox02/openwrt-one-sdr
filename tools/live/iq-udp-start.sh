#!/bin/sh
# Router-side controller. UDP destination must be the directly connected host.
# Usage: iq-udp-start.sh IFACE SRC_IP DST_IP DST_MAC PORT PACKETS [BW=0] [FREQ] [FMT] [BAND=0]
#   FREQ  RF centre in Hz (default 2437000000); FMT iqd1|vrt8|vrt16 (default iqd1).
set -eu
[ "$#" -ge 6 ] || { echo "usage: $0 IFACE SRC_IP DST_IP DST_MAC PORT PACKETS [BW=0] [FREQ] [FMT] [BAND=0]" >&2; exit 2; }
iface=$1 src=$2 dst=$3 mac=$4 port=$5 packets=$6 bw=${7:-0} freq=${8:-2437000000} fmt=${9:-iqd1} band=${10:-0}
case "$band" in 0|1) ;; *) echo "BAND must be 0 (2.4 GHz) or 1 (5 GHz)" >&2; exit 2;; esac
# Rates require the explicit path0 packing below; stale packing changes buffer interpretation.
case "$bw" in 0) rate=40000000;; 1) rate=80000000;; *) exit 2;; esac
case "$fmt" in iqd1|vrt8|vrt16) ;; *) echo "bad FMT: $fmt" >&2; exit 2;; esac
[ "$rate:$fmt" != '80000000:vrt16' ] || { echo '80 MSps in int16 I/Q requires 2.56 Gb/s before overhead; use vrt8' >&2; exit 2; }
# Refuse metadata that disagrees with the tuned, verified radio profile.
profile=$(/root/iq-radio.sh --profile) || { echo 'Cannot verify active radio profile' >&2; exit 1; }
set -- $profile
[ "$freq:$rate:$band:$bw" = "$1:$2:$4:$5" ] || {
    echo "Requested capture differs from active profile: $profile" >&2; exit 2;
}
[ -x /root/iq-sched ] || { echo 'Build and install tools/live/iq-sched.c first' >&2; exit 1; }
. /root/icap-device.sh
[ -e "$D/iq_udp" ] || { echo 'UDP export driver missing' >&2; exit 1; }
if ! grep -q '^stopped' "$D/iq_udp"; then /root/iq-udp-stop.sh; fi
echo stop > "$D/iq_udp"
# Prime once, retire, then leave the dedicated pool running for the entire run.
kb 0; sw 4 1; sw 16 2113536; sw 20 4096; sw 24 4096; sw 40 "$band"; sw 44 "$bw"; sw 56 "$band"
kb 0x1400000000000000
sleep 1
native_retire "$band"
# Pack the first two lane words. On 5 GHz these are successive path0
# samples; on the tested 2.4 GHz HE40 profile they alternate RF paths.
# Stock firmware only sets packer bits; never inherit a previous capture's mode.
cur=$(rr 0x800230b8)
wr 0x800230b8 "$(printf '0x%x' "$(( (cur & ~0xf0000) | 0x10000 ))")"
[ "$(( $(rr 0x800230b8) & 0xf0000 ))" -eq 65536 ] || exit 1
wr 0xe009b800 0
wr 0xe009b804 0
if [ "$band" = 0 ]; then stride=8; else stride=4; fi
echo "$iface $src $dst $mac $port $rate $freq $fmt $stride" > "$D/iq_udp"
# Older drivers silently ignore a ninth token. Verify support before starting
# the producer so an old module cannot emit mixed lanes with plausible metadata.
if ! grep -q " stride=$stride " "$D/iq_udp"; then
    echo stop > "$D/iq_udp"
    echo 'Exporter lacks verified native-stride support; install the matching driver' >&2
    exit 1
fi
# Put the MAC in receive-only mode on both bands (ARB_SCR TX_DISABLE, bit 8): the
# radio keeps receiving to feed the capture, but never transmits, so beacons and
# probe responses do not contend with the capture drain. Receive-thread
# scheduling is also required; residual stalls remain under investigation.
for arb in 0x820e3000 0x820f3000; do
	cur=$(rr "$arb") || continue
	wr "$arb" "$(printf '0x%x' "$(( cur | 0x100 ))")"
done
# Synchronous 115200-baud console output can block RX longer than the pool.
# Preserve all messages in dmesg/syslog; suppress ordinary console output only.
cat /proc/sys/kernel/printk > /tmp/iq-printk.saved
echo 1 > /proc/sys/kernel/printk
# A normal-priority threaded NAPI receiver can sleep longer than the 1.64ms
# capture pool. Keep its wakeups prompt, on CPU0, away from the CPU1 converter.
rx_pid=$(sed -n 's/.* rx_pid=\([0-9]*\).*/\1/p' "$D/iq_udp")
if [ -z "$rx_pid" ] || ! /root/iq-sched "$rx_pid" 0 10; then
	echo stop > "$D/iq_udp"
	cat /tmp/iq-printk.saved > /proc/sys/kernel/printk
	rm -f /tmp/iq-printk.saved
	echo 'Cannot prioritize MCU receive thread; capture not started' >&2
	exit 1
fi
kb 0; sw 0 6; sw 4 "$packets"; kb 0x1400000000000000
# re-assert TX-disable in case the prime touched the MAC state
for arb in 0x820e3000 0x820f3000; do cur=$(rr "$arb") && wr "$arb" "$(printf '0x%x' "$(( cur | 0x100 ))")"; done
echo "Capture started: $packets raw packets, nominal $rate samples/s/path, fmt=$fmt freq=$freq band=$band (MAC rx-only)."
echo "Inspect $D/iq_udp; stop acquisition with: wr 0xe009b840 1"
