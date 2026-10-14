# tools/webui — live (gapped) spectrum viewer

A local, offline web page showing **live IQ from the OpenWrt One's WiFi radio**:
spectrum (both antenna paths + peak hold), waterfall, and constellation. A
bring-up probe, separate from the streaming viewer in [../../host/spectrum/](../../host/spectrum/README.md).

## How it works

```
router: tools/live/iqd (C daemon)        host: owrt_sdr_webui.py          browser
  native capture -> poll -> retire         decode banks/paths (numpy)  -HTTP-> spectrum
  fire DMA producer (EXT56 func 3)  -TCP-> PSD per path, stats                 waterfall
  read 8x 0xf1 relay events, loop          /api/latest                        constellation
```

`iqd` runs on the router and serves `IQ01` + a 64 KiB native window per frame
over **TCP :7700** at ~50 fps (DMA export, not the 209 ms register read). The
server connects to it, and still SSHes once for the neighbour-AP scan and the
centre frequency. Each 64 KiB window decodes with the MT7981 layout: two 32 KiB
banks; each 16-byte beat = low 8 bytes from bank0 + high 8 from bank1;
path0 = words 0,2, path1 = words 1,3; I = high16 & 0x3fff, Q = low16 & 0x3fff,
signed 14-bit.

Build + install iqd: `tools/live/build-iqd.sh 192.168.68.70`. The router needs
port 7700 open (a `src=wan` uci firewall rule, since the uplink is the wan zone)
and the diagnostic driver + window-DMA firmware loaded.

## Reading the display

- **Trace** defaults to *burst*: the per-bin 95th percentile of the last 30
  frames. WiFi is bursty, so a plain average buries packets; the percentile
  shows each transmitting AP as a ~17 MHz hump with a sharp notch at its centre
  (the 802.11 OFDM null subcarrier). *average* and *latest frame* are also
  available.
- **AP markers** come from a one-time `iw scan` on the router at startup
  (`--no-scan` to skip): SSID and signal at each neighbour's channel centre.
- The **spike at the exact centre** is the receiver's own DC/LO leakage, not a
  signal. The off-centre constellation cloud is the same DC offset.
- **Full-width stripes** in the waterfall are mostly AGC gain changes between
  snapshots.

## Validation

With the radio retuned from ch3 to ch4, the neighbour APs at 2412 and 2432 MHz
stay at the same absolute frequency while the receiver centre moves 5 MHz, the
pattern is not mirrored, and each AP's OFDM centre notch lands within one FFT
bin of its channel centre. That confirms real RF, the 40 MSps axis, and the
spectrum orientation.

## Limits (honest)

- **Gapped, not continuous.** Each frame is one ~8192-sample snapshot per path
  (~200 µs at 40 MSps), about **6 frames/s**. It is a live spectrum analyser,
  not a gap-free stream.
- Levels are in dBFS after the receiver's AGC, which levels each packet to a
  similar target; they are not calibrated power.
- **SSH is a bring-up transport.** The real transport is a UDP sender in the
  W1700K-SDR `w1700k_stream.h` wire format.
- Each `icap_window` read logs one `dev_info` line to the kernel log.

## Run

Router prerequisites: the diagnostic `mt7915e.ko` (`icap_prime`, relay patch)
and the window-DMA firmware loaded, normal boot, radios up, `/root/iqd` present
(`../live/build-iqd.sh`), port 7700 open.

```sh
python3 owrt_sdr_webui.py --host 192.168.68.70 --key ~/.ssh/owrt_one
# open http://127.0.0.1:8088/
```

Options: `--iqd-host`/`--iqd-port` (data TCP, default `--host`:7700 — point this
at the 2.5 GbE direct-wire address), `--fft`, `--fs` (assumed sample rate),
`--centre`, `--port`. The viewer starts iqd over SSH if it is not already
running. Needs numpy on the host; the page loads nothing from the internet.

## Files

- `owrt_sdr_webui.py` — iqd TCP client, decoder, `/api/latest` JSON.
- `index.html` — spectrum, waterfall, constellation, live stats.
- `../live/iqd.c` — the router-side capture+DMA+TCP daemon (the fast path).
- `../live/iqloop.sh` — the older SSH shell loop (slow fallback, ~6 fps).
