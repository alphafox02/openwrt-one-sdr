<!-- SPDX-License-Identifier: MIT -->
# openwrt-one-sdr v0.1.0

Turns an OpenWrt One into a wideband, receive-only SDR: the MT7981 Wi-Fi radio
captures 14-bit IQ at 40 Msps on 2.4 and 5 GHz, streams it as VITA 49 over the
2.5 GbE port, and a Linux host shows it as a live spectrum and waterfall.

## Get going

1. **Flash** `openwrt-one-sdr-v0.1.0-squashfs-sysupgrade.itb` to the board (a
   normal OpenWrt sysupgrade). It boots to its LAN at `192.168.1.1`.
2. **Set up** from your host — unpack `openwrt-one-sdr-v0.1.0-sdr-tools.tar.gz`
   and run `./setup.sh <board-lan-ip>` (needs only `ssh`/`scp` and Python 3),
   then reboot the board.
3. **View** — build the host viewer/bridge and open the browser UI, as in the
   project README "Getting started".

Prefer to build it all yourself? Follow README path B (`docs/reproduce.md` +
`init/deploy.sh`).

## About the firmware

No MediaTek firmware is distributed here. The image carries the **stock** blob
exactly as OpenWrt ships it; `setup.sh` patches a copy of your board's **own**
stock firmware locally (toolchain-free, via `apply-producer.py`) into the SDR
firmware, which never leaves your machine. See `release/README.md`.

## Assets

- `openwrt-one-sdr-v0.1.0-squashfs-sysupgrade.itb` — flashable image (stock fw + SDR driver)
- `openwrt-one-sdr-v0.1.0-sdr-tools.tar.gz` — host bring-up bundle (no toolchains)
- `SHA256SUMS`

## Known limits

40 Msps with ~20 MHz usable passband; Wi-Fi channel-grid tuning (snap to nearest
centre); 5 GHz limited to U-NII-1; ~25 s retune; residual overruns under
sustained capture. Details in the README "Design approach and limitations".
