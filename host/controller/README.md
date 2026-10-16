# controller

Receiver control: a VITA 49.2 command subset (for SoapySDR/VITA 49.2
applications) and a network adapter that lets the vendored spectrum viewer drive
the board over the on-device `iq-c2d` daemon — no SSH. Live SoapyVITA49 frequency commands and resumed Ci16 reads passed across both
bands using the timeout-fixed client and explicit C2 capture start/stop. Full
VITA conformance has not been established.

| File | Role |
| --- | --- |
| [vita49-control.c](vita49-control.c) | ordered, single-field command execution, configured-state queries and UUID echo |
| [vita49-apply.sh](vita49-apply.sh) | validates HE40 block centres (2422–2462 MHz in 5 MHz steps; 5190/5230 MHz), stops/drains capture, applies the channel and waits 25 s |
| [adapter.py](adapter.py) | viewer Start/Stop/Tune over `iq-c2d` (UDP, no SSH), snapping any requested frequency to the nearest supported channel |
| [test/test_control.py](test/test_control.py) | loopback packet-layout tests with a mock hook |

## Supported subset

Big-endian, type-6 command packets with no class ID, timestamps, scheduling or
extended CIFs. Both 32-bit and 128-bit controller/controllee IDs are preserved.
Only CIF0 bandwidth, RF frequency, stage-1 gain and sample rate are parsed.
Mutations must contain exactly one field; queries may request several.

The responder calls `HOOK --check FIELD VALUE` before execution or dry-run.
A successful execute then calls `HOOK FIELD VALUE` and waits for completion;
only then is configured state updated and a success acknowledgement sent.
Hooks must make validation side-effect-free. Each hook call has a 45-second
limit. Fractional values are preserved.

The supplied hook tunes HE40 block centres: 2422–2462 MHz in 5 MHz steps on
2.4 GHz and 5190/5230 MHz on 5 GHz. The viewer snaps requests to these centres.
The 2.4 GHz profile uses monitor mode; an actual-width fallback to 20 MHz fails
profile validation. Unchanged 40 Msps and 40 MHz requests succeed; rate/width
changes and gain writes are unsupported. An RF retune stops capture. The next
start uses the actual block centre in VITA context; the viewer restarts an
active stream after settling.

Unsupported commands and failed hooks currently produce a controller timeout,
not a structured VITA error acknowledgement. A failure during execution makes
hardware state uncertain, so subsequent queries are also withheld until the
responder is restarted with verified settings. With the deployed `--readback` option, queries read the current operating
centre and width from the radio helper. This does not measure PLL lock. Without
that option, state is cached configuration; unknown fields are omitted.

RF execution takes at least 25 seconds; a controller's normal subsecond timeout
is insufficient. For the local SoapyVITA49 implementation, `ctrl_timeout=45000`
sets a longer timeout in milliseconds. Use a SoapyVITA49 build that applies this timeout to field writes, not only its
initial probe. Older builds may ignore this setting for writes. The timeout-fixed
client passed 2437 → 5190 → 2437 MHz with complete sample reception after each
retune; acquisition start/stop used C2.
There is no duplicate-message cache; retries can repeat an operation.

## Build and test

```sh
cc -O2 -Wall -Wextra -Werror -o host/controller/vita49-control host/controller/vita49-control.c
python3 -I host/controller/test/test_control.py
python3 -I tests/test_offline_fixes.py
```

On the board, cross-compile the responder with the OpenWrt toolchain. The
[boot service](../../init/README.md) derives its initial RF centre from UCI.
The default UDP command port is 4991. The custom `iq-c2d` session interface
(UDP 4992) separately provides discovery, start/stop and status.

These loopback tests exercise packet layout and failure handling. They do not
prove end-to-end interoperability, hardware retuning or sustained streaming.

## Licence

MIT, © 2026 CEMAXECUTER LLC. See [../../LICENSE](../../LICENSE).
