#!/bin/sh
# Stop the IQ capture/export and restore the MAC's transmit path on both bands
# (clear ARB_SCR TX_DISABLE, bit 8), so the radio can act as a normal AP again.
. /root/icap-device.sh
[ -n "${D:-}" ] || { echo 'ICAP device not found' >&2; exit 1; }
if grep -q '^stopped' "$D/iq_udp"; then echo 'export already stopped'; exit 0; fi
rx_pid=$(sed -n 's/.* rx_pid=\([0-9]*\).*/\1/p' "$D/iq_udp")
# Keep the exporter attached while the producer drains its last DMA transfer.
wr 0xe009b840 1 || exit 1
tries=0
while :; do
	status=$(rr 0xe009b800) || exit 1
	case "$((status))" in
		0|1) ;;
		4) echo 'DMA timeout: reset required; exporter retained' >&2; exit 1;;
		*) break;;
	esac
	tries=$((tries+1))
	[ "$tries" -lt 40 ] || { echo 'Producer did not stop; exporter retained' >&2; exit 1; }
	sleep 0.05
done
# Drain queued samples before detaching the exporter.
tries=0
while ! grep -q 'queued=0\|^stopped' "$D/iq_udp"; do
	tries=$((tries+1))
	[ "$tries" -lt 100 ] || { echo 'Export queue did not drain' >&2; exit 1; }
	sleep 0.05
done
echo stop > "$D/iq_udp" || exit 1
[ -z "$rx_pid" ] || /root/iq-sched "$rx_pid" 0 0
if [ -f /tmp/iq-printk.saved ]; then
	cat /tmp/iq-printk.saved > /proc/sys/kernel/printk
	rm -f /tmp/iq-printk.saved
fi
for arb in 0x820e3000 0x820f3000; do
	cur=$(rr "$arb") && wr "$arb" "$(printf '0x%x' "$(( cur & ~0x100 ))")"
done
echo "capture stopped; MAC transmit restored on both bands."
