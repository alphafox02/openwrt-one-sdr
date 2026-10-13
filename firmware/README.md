# firmware

Code that runs on the MT7981's Wi-Fi MCU (the "WM", an Andes nds32 core), and the
host-side mechanism that loads it into the stock firmware.

The capture engine writes IQ into an on-chip pool; a producer on the Wi-Fi MCU
drains it continuously. That producer, and the way custom code is placed into the
firmware without displacing MediaTek's, are here. Everything is assembled from
source with an `nds32le-elf` toolchain (GCC 10.5 + Binutils 2.42); see
[../docs/reproduce.md](../docs/reproduce.md).

| Directory | Contents |
| --- | --- |
| [streaming/](streaming/README.md) | the DMA producer (`window_dma.S`) and the tool that builds it into a firmware image. The working capture path. |
| [rom_patch/](rom_patch/README.md) | the code-execution mechanism: redirect a MediaTek runtime hook through a custom nds32 handler placed in the patch's padding gap. Proven on hardware; the producer builds on it. |
| [diagnostic/](diagnostic/README.md) | behaviour-preserving trace veneers used to decode the capture path against a running firmware. |

## The driver side

The capture is driven from a kernel module built in the OpenWrt mt76 tree (not
under `firmware/`): it adds the ICAP debugfs knobs and the zero-copy VITA 49 UDP
sender. See [../kernel/mt7915-icap/README.md](../kernel/mt7915-icap/README.md).

## Licence

MIT, © 2026 CEMAXECUTER LLC. The firmware code here is assembled from source and
contains no MediaTek code. See [../LICENSE](../LICENSE).
