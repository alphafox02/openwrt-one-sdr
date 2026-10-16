# tests

Host-side tests that run without the board.

| File | Checks |
| --- | --- |
| `test_vita49.py` | builds the VITA 49 emitter (`host/stream/iq-vita49.c`), sends a known IQ pattern as VRT over loopback, and verifies what a receiver decodes: the IF-Context fields (centre frequency, sample rate, data format) and a value-exact sample round-trip, for both Ci8 and Ci16, with a contiguous packet sequence. |

## Run

```sh
python3 tests/test_vita49.py        # prints PASS, exits 0 on success
```

Needs a C compiler (for the emitter) and Python 3. More coverage — the bridge's
shared-memory round-trip and the 14-bit packing math — is open work.
