# host/stream

Host-side tools for the VITA 49 IQ stream: bridge it into the spectrum viewer,
emit it, receive it, or check it.

| File | Role |
| --- | --- |
| `vita49-shm.c` | the bridge. Receives the VITA 49 UDP stream (batched with `recvmmsg`), converts each sample to the viewer's 14-bit scale, and publishes it into the `/w1700k-stream` shared-memory ring the viewer reads. Ci16 is shifted back from the 14-bit left-justified form; Ci8 is scaled up. |
| `vita49-shm-flat.cpp` | the bridge with optional on-stream DSP: the passband flatten (honouring the viewer's shared-memory flatten word) and an opt-in DC blocker (`--dc-block`). Use in place of `vita49-shm.c` when either correction is wanted. |
| `flatten.{h,cpp}` | the decimation roll-off correction (a fixed linear-phase FIR, overlap-save). Vendored from W1700K-SDR (MIT); the per-rate roll-off tables are re-measured for the MT7981. |
| `iq-vita49.c` | a VITA 49 emitter. Reads interleaved IQ (Ci8 or Ci16) from a file or stdin and sends it as VRT over UDP — a periodic IF-Context packet plus Signal-Data packets. Used to replay or test against any VITA 49 receiver. |
| `vrt-save.c` | a minimal lossless VITA 49 receiver. Writes the decoded int16 IQ to a file for offline analysis and prints the detected centre frequency, sample rate and data format. |
| `vrt-check.c` | sequence and integrity check over a received VITA 49 stream. |
| `iq-udp-recv.c` | receiver for the legacy fixed-header (IQD1) datagram format. |
| `include/w1700k_stream.h` | the shared-memory ring format the viewer and the bridge share. Vendored from W1700K-SDR (MIT). |

## Build

```sh
cc -O2 -I include -o vita49-shm vita49-shm.c -lrt
cc -O2 -o iq-vita49 iq-vita49.c
cc -O2 -o vrt-save  vrt-save.c
c++ -O2 -std=c++17 -I . -o vita49-shm-flat vita49-shm-flat.cpp flatten.cpp -lfftw3f -lrt
```

## VITA 49 framing

Big-endian, bare VRT (no VRL wrapper). The emitter and the kernel sender both
produce:

- **IF-Context** (packet type 0x4), sent first and periodically: a CIF0 word with
  RF reference frequency, sample rate (both 64-bit, Hz × 2^20) and data-packet
  payload format. Ci8 = complex / signed-int / 8-bit; Ci16 = 16-bit; packed 14-bit
  = complex / signed-int / 14-bit, link-efficient (payload-format bit 31 set).
- **Signal-Data** (packet type 0x1): a stream ID, then the interleaved IQ payload.

A receiver auto-detects centre frequency, sample rate and format from the
IF-Context packet. Ci16 carries the full 14-bit capture resolution left-justified
into int16; the packed 14-bit profile carries the same samples with no padding —
28 bits per complex sample, I then Q, each MSB-first — which is lossless and about
12 % smaller than Ci16 (the headroom that lets a higher rate fit the link). The
bridges decode all three; the packer that emits the 14-bit profile on the board
is a separate, still-unbuilt producer change. This project's packed14 subset
requires eight-sample groups (payload length a multiple of28bytes), with no tail
padding. It does not carry a pad-bit count. Arbitrarily padding a seven-sample
payload would be indistinguishable from an eight-sample payload; senders must
not do that. This restriction is specific to this parser/profile, not a claim
that VITA disallows other packet sizes.

The bridges accept this project's exact context layout and type-1 data profile,
with no class IDs, timestamps or trailer. Unsupported profiles and truncated
packets are rejected. Context must arrive before samples; a stream ID mismatch
is rejected. `bad` counts rejected datagrams. Ci16 scaling assumes this
project's native 14-bit samples left-justified in a 16-bit word.

Detected four-bit packet-count discontinuities, metadata changes, long pauses
and viewer restart requests begin a new shared-memory run and discard partial
slices. The DSP bridge also resets FIR/DC history and selects its flatten
filter from the received rate. Losses of exactly 16 packets can be invisible;
there is no socket-overflow ancillary or source sample-count accounting yet.
These bridges do not establish gap-free acquisition. See
[control-plane limits](../../docs/control-plane.md).

## On-stream corrections (`vita49-shm-flat`)

Two optional corrections, both applied to the IQ before it reaches the ring, so
every consumer benefits — not just the viewer's display.

- **Flatten the passband.** The decimation rolls the band off toward the edges
  (measured on the MT7981: flat to ~42 % of Nyquist, about −33 dB at the edge).
  `flatten.cpp` divides that roll-off out with a fixed linear-phase FIR, so it
  does not suppress real signals. The bridge follows the viewer's flatten word
  in the shared-memory header, so the viewer's checkbox toggles it live.
- **DC blocker (`--dc-block`).** Direct-conversion leaves an LO offset as a
  spike at 0 Hz. A per-component running mean subtracted from each sample
  removes it in the IQ domain (about a 64 Hz corner at 40 Msps, also
  suppressing genuine signals near DC), unlike the viewer's
  display-only centre-bin patch, which leaves the offset's leakage in nearby
  bins.

```sh
vita49-shm-flat --port 4991 --ring 16384 --dc-block
```

## Shared-memory ring

`vita49-shm` acts as the producer side of the `w1700k_stream.h` ring: a 4 KiB
header, then one 64-bit tag per slot, then slots of 2048 complex int16 samples.
Slots are published in order under a seqlock; the viewer follows the frontier.
The bridge refreshes the ring heartbeat on a timer, so a gap in the stream does
not read as a dead service.

## Licence

MIT, © 2026 CEMAXECUTER LLC, except `include/w1700k_stream.h` and
`flatten.{h,cpp}` (vendored from W1700K-SDR, MIT, © 2026 h0m3us3r; the
MT7981 roll-off tables in `flatten.cpp` are this project's). See
[../../LICENSE](../../LICENSE).
