# firmware/streaming

The DMA producer that runs inside the MediaTek Wi-Fi firmware, and the tool that
builds it into a firmware image.

The MT7981 ICAP capture engine writes 14-bit IQ into an on-chip pool. A producer
running on the Wi-Fi MCU (nds32) must drain that pool continuously, or it
overruns. This is that producer, injected into the stock firmware at build time.

| File | Role |
| --- | --- |
| `window_dma.S` | the nds32 producer. Follows the ICAP write pointer (CR9), drains each 8 KiB chunk of the capture pool into the MCU's TX0 DMA ring, and emits it to the host as an MCU extension event (`ext_eid` 0xf1), which the driver turns into a network skb. Runs with interrupts masked for the duration of a capture, so no firmware task preempts the drain. Several entry functions: the continuous producer (func 6), a bounded window export (func 3), and a dedicated-pool export (func 5). |
| `build.py` | builds the patched firmware. Reads the stock MediaTek WM image (supplied by the running system, never committed here), assembles `window_dma.S` for nds32, writes it into an unused region of the firmware and redirects the command dispatcher to it, repairs the PDA and container CRCs, and with `--pool` selects the UMAC partition profile that reserves the dedicated 512 KiB capture pool. |
| `apply-producer.py` | the same patch without a toolchain. Carries the producer **pre-assembled** (this project's own bytes, embedded as base64) and applies the identical edits, PDA repair and CRC to a stock image, so a user on any machine with Python 3 can produce the SDR firmware from the stock blob on their own board. Output is byte-identical to `build.py --pool`. This is what the release `setup.sh` runs. |

## Build

```sh
python3 firmware/streaming/build.py <out-dir> --pool
# writes <out-dir>/wm-window-dma.bin, the firmware to load on the board
```

The nds32 assembler and linker come from the cross toolchain (GCC 10.5 + Binutils
2.42); see [docs/reproduce.md](../../docs/reproduce.md). The input firmware image
is read from `vendor/` (kept out of the repository by `.gitignore`); the build
asserts its MD5 so the patch applies to the expected image.

Without the toolchain — the path a release user takes:

```sh
# stock_wm.bin is the copy from your board's /lib/firmware/mediatek/mt7981_wm.bin
python3 firmware/streaming/apply-producer.py stock_wm.bin wm-sdr.bin
```

Both `build.py` and `apply-producer.py` read a stock image and emit the patched
one; neither ships MediaTek code, and the patched firmware is produced locally
and never redistributed.

## Hazards

- The producer runs on the Wi-Fi MCU in firmware context. A wrong address or an
  overlong masked-interrupt window can wedge the Wi-Fi subsystem and force a
  reboot.
- A plain reboot reverts the firmware to stock; the image must be re-flashed.
- The capture pool is 512 KiB and is drained at the capture rate; the drain must
  keep up or the pool overruns (status 5). The ring extent is set in
  `window_dma.S`; it is bounded by the UMAC partition the `--pool` patch selects.

## Licence

MIT, © 2026 CEMAXECUTER LLC. The firmware code here is assembled from source in
this repository and contains no MediaTek code. See [../../LICENSE](../../LICENSE).
