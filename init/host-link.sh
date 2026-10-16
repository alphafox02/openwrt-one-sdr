#!/bin/sh
# SPDX-License-Identifier: MIT
# Bring up the host's 2.5 GbE interface for the direct link to the board's WAN
# (eth0) port, where the IQ stream and control arrive.
#
#   init/host-link.sh IFACE [HOST_IP/CIDR]
set -eu
IFACE=${1:?usage: host-link.sh IFACE [HOST_IP/CIDR]}
ADDR=${2:-10.77.0.11/24}
ip addr replace "$ADDR" dev "$IFACE"
ip link set "$IFACE" up
echo "host interface $IFACE up at $ADDR"
echo "the board's eth0 should be 10.77.0.10/24 on the same cable;"
echo "this helper sets a temporary host address; the saved NetworkManager profile is persistent."
echo "if board IPv4 recovery is needed, its IPv6 link-local fallback is:"
echo "  ssh -6 root@fe80::2205:b6ff:feff:a5bc%$IFACE 'ip addr add 10.77.0.10/24 dev eth0'"
