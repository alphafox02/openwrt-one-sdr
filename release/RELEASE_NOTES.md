<!-- SPDX-License-Identifier: MIT -->
# openwrt-one-sdr v0.2.0

Turns an OpenWrt One into a wideband, receive-only SDR: the MT7981 Wi-Fi radio
captures 14-bit IQ at 40 Msps on 2.4 and 5 GHz, streams it as VITA 49 over the
2.5 GbE port, and a Linux host shows it as a live spectrum and waterfall.

## New in v0.2.0

- **40 MHz channel width on both bands.** The radio now comes up HE40 (2.4 GHz
  in monitor mode to keep the full 40 MHz), so the 40 Msps capture fills the
  window instead of ~20 MHz. Confirmed on both bands in bounded on-hardware
  tests with zero counted drops.
- Correct per-band native sample lanes in the driver and capture helper (a
  previous stride mismatch mis-reported 2.4 GHz timing), tune reports the
  verified HE40 block centre, and the default setup brings up a 40 MHz block.

## Get going

1. **Flash** `openwrt-one-sdr-v0.2.0-squashfs-sysupgrade.itb` to the board (a
   normal OpenWrt sysupgrade). It boots to its LAN at `192.168.1.1`.
2. **Set up** from your host — unpack `openwrt-one-sdr-v0.2.0-sdr-tools.tar.gz`
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

- `openwrt-one-sdr-v0.2.0-squashfs-sysupgrade.itb` — flashable image (stock fw + SDR driver)
- `openwrt-one-sdr-v0.2.0-sdr-tools.tar.gz` — host bring-up bundle (no toolchains)
- `SHA256SUMS`

## Known limits

40 Msps; Wi-Fi channel-grid tuning (snap to nearest HE40 block centre); 5 GHz
limited to U-NII-1; ~25 s retune; residual overruns under sustained capture.
Calibrated passband flatness and indefinite gap-free operation remain unproven;
80 Msps is not a validated 80 MHz receiver. Details in the README "Design
approach and limitations".
