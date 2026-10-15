# controller

Receiver control: a VITA 49.2 command subset (for SoapySDR/VITA 49.2
applications) and a network adapter that lets the vendored spectrum viewer drive
the board over the on-device `iq-c2d` daemon — no SSH. Live SoapyVITA49
interoperability and full VITA conformance have not been established.

| File | Role |
| --- | --- |
| [vita49-control.c](vita49-control.c) | ordered, single-field command execution, configured-state queries and UUID echo |
| [vita49-apply.sh](vita49-apply.sh) | validates exact 2.4 GHz (1–13) and 5 GHz (36/40/44/48) channel centres, stops/drains capture, applies the channel and waits 25 s |
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
Hooks must make validation side-effect-free. Each hook call has a 35-second
limit. Fractional values are preserved.

The supplied board hook implements exact 2.4 GHz (channels 1–13) and 5 GHz
(36/40/44/48, U-NII-1) channel centres; the viewer adapter snaps arbitrary
requests to these. Channel availability still depends on the board's
configuration. Rate, bandwidth and gain writes fail validation; no hardware
changes are claimed. An RF retune leaves capture stopped. Its next start must
use the new frequency in the exported context. The viewer adapter restarts
capture after settling.

Unsupported commands and failed hooks currently produce a controller timeout,
not a structured VITA error acknowledgement. A failure during execution makes
hardware state uncertain, so subsequent queries are also withheld until the
responder is restarted with verified settings. Query state represents supplied
startup RF/rate configuration and completed hook operations. Unknown startup
gain/bandwidth fields are omitted. This is not measured PLL lock or
a hardware readback. Controls through other processes can make it stale.

RF execution takes at least 25 seconds; a controller's normal subsecond timeout
is insufficient. For the local SoapyVITA49 implementation, `ctrl_timeout=35000`
sets a longer timeout in milliseconds. Live behavior still needs verification.
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
