# host/capture

Host-side probes that read a capture buffer directly, used during bring-up and
reverse engineering before the network stream existed. The streaming path in
[../stream/](../stream/README.md) supersedes these for normal use; they remain
for inspecting the raw capture.

| File | Role |
| --- | --- |
| `window_dump.c` | reads a retired capture window out of the radio's SRAM through the mt76 debugfs register proxy, for offline decode. |
| `decode_dma.py` | validates the bounded eight-packet DMA export against the SRAM window, byte for byte — the check that first proved the DMA path carried real samples. |
| `iqr_recv.c` | an early host consumer for the continuous IQ firehose, before the VITA 49 framing. |

## Licence

MIT, © 2026 CEMAXECUTER LLC. See [../../LICENSE](../../LICENSE).
