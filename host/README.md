# host

The computer side of the link: receive the IQ stream off the board, convert it
for the spectrum viewer, and record or re-emit it.

The board sends VITA 49 over UDP. The viewer reads a shared-memory ring in the
`w1700k_stream.h` format. A bridge sits between them, so the viewer runs exactly
as published upstream.

| Directory | Contents |
| --- | --- |
| [stream/](stream/README.md) | the VITA 49 → shared-memory bridge (`vita49-shm`), a VITA 49 emitter (`iq-vita49`), a lossless receiver (`vrt-save`), and the stream-ring header |
| [spectrum/](spectrum/README.md) | the spectrum and waterfall viewer: a credited MIT variant vendored from W1700K-SDR with minimal changes (page title) |
| [capture/](capture/README.md) | bring-up probes that read a capture buffer directly, before streaming is up |
| [controller/](controller/README.md) | receiver control: the VITA 49.2 command responder (`vita49-control`) and a network adapter that drives the viewer over the board's C2 daemon (no SSH) |

## Data path

```
board ── VITA 49 UDP ──▶ vita49-shm ── /w1700k-stream (shm ring) ──▶ w1700k-spectrum ──▶ browser
```

`vita49-shm` (host/stream) receives the VITA 49 datagrams, converts the samples
to the viewer's native 14-bit scale, and publishes them into the shared-memory
ring. `w1700k-spectrum` (host/spectrum) maps that ring, runs the FFT, and serves
the browser UI. The ring format decouples the two: the same viewer serves any
producer that fills the ring.

## Why VITA 49

VITA 49 (VRT) is an open standard for carrying IQ samples and their metadata.
Emitting it means the stream drops into existing SDR software rather than a
bespoke format: a receiver reads the centre frequency, sample rate and data
format from the context packet and needs no prior knowledge of this device.
VITA 49.2 extends the same standard with a command/acknowledge control plane, so
tuning, sample rate and gain travel over the same open format as the data — no
separate control protocol to specify or re-implement per client. The alternative
on the board, `iq-c2d`, is a small custom protocol kept only for discovery and
session start/stop, which VITA 49 does not cover.

A VITA 49.2 controller is a separate piece of software; one example is
[SoapyVITA49](https://github.com/alphafox02/SoapyVITA49), a SoapySDR driver. The
responder's wire format was checked against that implementation's packet layout
(`host/controller/test/test_control.py`); end-to-end interop against a running
controller on real hardware has not yet been confirmed.

## Build

```sh
make -C host/spectrum                                  # zlib + FFTW3 (single precision)
cc -O2 -I host/stream/include -o vita49-shm host/stream/vita49-shm.c -lrt
```

## Licence

Host code here is MIT, © 2026 CEMAXECUTER LLC. The viewer under
[spectrum/](spectrum/) is vendored from W1700K-SDR (MIT, © 2026 h0m3us3r) and
keeps its own SPDX headers. See [../LICENSE](../LICENSE).
