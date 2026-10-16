# Receiver controls and current limits

The vendored spectrum viewer reads the shared-memory stream produced by
`host/stream/vita49-shm-flat.cpp`. Display controls run on the host; radio
controls require a working board implementation.

| Control | Current implementation | Verification limit |
| --- | --- | --- |
| Start/stop | the on-device C2 daemon (`iq-c2d`); the viewer adapter drives it over the network, no SSH | strict uninterrupted capture still fails intermittently |
| RF tune | exact 2.4 GHz (1–13) and 5 GHz (36/40/44/48) centres through the VITA apply hook; the viewer adapter snaps any requested frequency to the nearest | stops/drains capture, reloads Wi-Fi, waits 25 s; live retune validation pending |
| Band (2.4/5 GHz) | selected by tuning into the band; 2.4 GHz and 5 GHz U-NII-1 (36/40/44/48) at 40 Msps | bounded runs validated on both; sustained long-run continuity still intermittent |
| Sample rate | demonstrated capture configuration: 40 Msps | VITA rate writes are rejected; 80 Msps overruns the current path |
| Analog bandwidth | existing priming configuration | VITA bandwidth writes are rejected |
| Gain and trim | no implemented manual RF control | requests fail; no successful adjustment is claimed |
| Flatten | rate-selected host FIR using the received context | filter availability does not prove the corresponding radio rate works |
| DC removal | optional bridge `--dc-block` running-mean subtraction | suppresses real signals close to DC as well as LO offset |
| FFT/display | vendored viewer | display smoothness does not prove sample continuity |

The VITA responder implements a restricted command subset. It validates and
serializes single-field hook operations, preserves UUIDs, and acknowledges only
completed operations. Unsupported controls and failures currently time out;
structured VITA error packets remain unfinished. Reported state is configuration,
not independent hardware readback. Live SoapyVITA49 interop is pending. See
[the controller documentation](../host/controller/README.md).

The radio's current retune path has a gap and at least a 25-second settle wait.
The VITA hook leaves acquisition stopped; the next start must export the new
centre frequency. The viewer adapter (over `iq-c2d`, no SSH) restarts a
previously active stream after settling and reports command failures.

## Bandwidth and transport

40 Msps is the demonstrated operating configuration, not a proven fundamental
silicon ceiling. The current capture path writes both RF streams into a small
on-chip pool; prior 80 Msps trials overran that pool. Solving that acquisition
bottleneck is separate from proving that the host/network path can drain it.

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
