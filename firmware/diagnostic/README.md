# firmware/diagnostic

A behaviour-preserving trace image for the stock Wi-Fi firmware, used to observe
the capture path without changing it.

It adds diagnostic veneers that keep the original firmware behaviour intact but
expose state to the host: a mailbox in the firmware's zero padding, a periodic
callback that publishes the global pointer, and a command hook that traces the
real ICAP dispatcher. This is how the capture command path was decoded against a
running trace, before any functional patch.

| File | Role |
| --- | --- |
| `trace.S` | the nds32 veneers: mailbox, periodic gp export, ICAP dispatcher trace. |
| `build_trace.py` | builds the trace image against the pinned stock firmware (asserts its hash). |

## Licence

MIT, © 2026 CEMAXECUTER LLC. See [../../LICENSE](../../LICENSE).
