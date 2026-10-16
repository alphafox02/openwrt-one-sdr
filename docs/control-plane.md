# Receiver controls and current limits

The vendored spectrum viewer reads the shared-memory stream produced by
`host/stream/vita49-shm-flat.cpp`. Display controls run on the host; radio
controls require a working board implementation.

| Control | Current implementation | Verification limit |
| --- | --- | --- |
| Start/stop | the on-device C2 daemon (`iq-c2d`); the viewer adapter drives it over the network, no SSH | bounded clean captures pass; indefinite continuity unproven |
| RF tune | HE40 block centres: 2422–2462 MHz in 5 MHz steps, 5190/5230 MHz through the VITA apply hook; the viewer adapter snaps any requested frequency to the nearest | stops/drains capture, reloads Wi-Fi, waits 25 s; readback checks operating centre and width |
| Band (2.4/5 GHz) | selected by tuning into the band; 2.4 GHz and 5 GHz U-NII-1 at 40 Msps | bounded runs validated on both; indefinite long-run continuity unproven |
| Sample rate | demonstrated capture configuration: 40 Msps | unchanged 40 Msps requests succeed; rate changes are rejected |
| RF channel width | HE40, checked against `iw`; 2.4 GHz uses monitor mode | unchanged 40 MHz requests succeed; width changes are rejected; calibrated passband is unmeasured |
| Gain and trim | no implemented manual RF control | requests fail; no successful adjustment is claimed |
| Flatten | rate-selected host FIR using the received context | filter availability does not prove the corresponding radio rate works |
| DC removal | optional bridge `--dc-block` running-mean subtraction | suppresses real signals close to DC as well as LO offset |
| FFT/display | vendored viewer | display smoothness does not prove sample continuity |

The VITA responder implements a restricted command subset. It validates and
serializes single-field hook operations, preserves UUIDs, and acknowledges only
completed operations. Unsupported controls and failures currently time out;
structured VITA error packets remain unfinished. The board hook reads the operating channel centre and width through `iw`
before commands and after applies. This is not a PLL-lock measurement. Live
Soapy frequency commands and resumed Ci16 reads passed across both bands with
the timeout-fixed plugin and explicit C2 acquisition start/stop. See
[the controller documentation](../host/controller/README.md).

The radio's current retune path has a gap and at least a 25-second settle wait.
The VITA hook leaves acquisition stopped; the next start must export the new
centre frequency. The viewer adapter (over `iq-c2d`, no SSH) restarts a
previously active stream after settling and reports command failures.

## Bandwidth and transport

40 Msps is the operating configuration on both HE40 profiles. The capture
packer and exporter must agree on native sample lanes: 2.4 GHz selects every
other packed word, while 5 GHz uses every word. A shared stride previously
mixed the 2.4 GHz lanes and advertised the wrong sample timing.

The kernel exporter copies and converts samples into network packets. It avoids
the userspace relay but is not a zero-copy capture-to-Ethernet DMA path. MAC
TX-disable reduced contention without eliminating residual stalls or losses.

At 80 Msps, 16-bit I plus 16-bit Q requires 2.56 Gb/s before packet overhead,
which exceeds 2.5 GbE. Packing the native 14-bit components would reduce sample
payload to 2.24 Gb/s, but such transport is not implemented or validated here.
Moving to 5 GHz alone does not remove these capture, CPU or link constraints.

## Continuity in the viewer

Both bridges now validate the project's timestamp-free Ci8/Ci16 VRT profile,
require context before data, and start a new shared-memory run on detected
packet-count gaps, changed metadata, explicit restart requests or long pauses.
The DSP bridge clears buffered FIR/DC state at those boundaries. It selects
its filter from the received rate instead of assuming 40 Msps.

The VRT packet counter has only four bits. A loss of exactly 16 packets, or a
restart with identical metadata and matching count, can remain invisible.
Socket-overflow ancillary accounting and source-level sample counters are not
implemented in these bridges. They cannot certify gap-free acquisition.
Strict end-to-end tests and firmware/driver counters remain necessary.

`iq-c2d PROFILE` includes `bandwidth=40000000` (RF channel width in Hz).
Its existing `bw=0` field is the capture-command selector, not RF width.
Older daemons without `bandwidth` give the viewer an unknown width, not a
hardcoded 20 MHz value.
