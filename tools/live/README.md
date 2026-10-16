# tools/live

On-device control for a live capture: prime the engine, hold the MAC
receive-only, start and stop the stream, and (optionally) drive it over the
network.

These run on the board (BusyBox `ash`). They use a small set of register
helpers sourced from `/root/icap-device.sh` (`rr`/`wr` read and write chip
registers through the mt76 debugfs proxy; `kb`/`sw` poke the ICAP command).

| File | Role |
| --- | --- |
| `iq-udp-start.sh` | the main entry point. Primes the native capture, puts the MAC in receive-only mode on both bands (ARB_SCR TX-disable, so beacons and probe responses cannot contend with the capture drain), configures the kernel exporter to stream VITA 49 to a host, prioritises the receive thread, and starts the producer. |
| `iq-udp-stop.sh` | stops the producer, waits for it to drain, detaches the exporter, and restores the MAC transmit-enable bits; a monitor interface remains a monitor. |
| `iq-sched.c` | sets scheduling priority and CPU affinity for a thread (the kernel receive thread), so its wakeups stay inside the capture-pool budget. |
| `iq-c2d.c` | the control daemon: a UDP service that answers discovery, streams back to whoever asks (reading the request's own source address), and broadcasts a beacon so a host finds the board regardless of DHCP. |
| `iqcontinuous.c`, `iqd.c` | earlier user-space capture readers, kept for reference; the kernel exporter supersedes them. |
| `iq-diagnostics.sh` | reads the diagnostic mailbox words after a run. |
| `build-iqd.sh`, `iqloop.sh` | build and loop helpers. |

## Run

```sh
# on the board: stream VITA 49 (receive-only MAC) to a host
iq-udp-start.sh eth0 <board-ip> <host-ip> <host-mac> 4991 40000000 0 2437000000 vrt8
# stop capture (monitor mode remains receive-only)
iq-udp-stop.sh
```

`iq-c2d` is cross-compiled with the OpenWrt toolchain and installed to `/root`;
see [../../docs/control-plane.md](../../docs/control-plane.md).

## Hazards

- A capture started immediately after `wifi up` or a radio reset overruns at
  once; let the radio settle (about 25 s) first.
- Register pokes touch live hardware; a wrong address can wedge the Wi-Fi
  subsystem.

## Licence

MIT, © 2026 CEMAXECUTER LLC. See [../../LICENSE](../../LICENSE).

## HE40 profile and native lanes

`iq-radio.sh --profile` returns centre Hz, sample rate, RF width Hz, band and
capture selector. Both profiles use HE40 and 40 Msps. The actual `iw center1`
value is the RF centre. 2.4 GHz uses monitor mode because AP coexistence can
force a requested HE40 configuration back to 20 MHz.

Deploy the updated driver and start script together. The exporter accepts a
ninth configuration token, native stride in bytes: 8 for this 2.4 GHz profile,
4 for 5 GHz. Both use CR10 64-to-128 packing, but the resulting native lane
layout differs. Ci16 retains all 14 captured bits on the selected path.
The default stride of 4 preserves the previous debugfs interface; older drivers
must not be used with the new 2.4 GHz helper.
