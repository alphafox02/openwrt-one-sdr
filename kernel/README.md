# kernel

The mt76 (mt7915) driver side: the patches that expose the ICAP capture engine
and stream it out as VITA 49, plus the priming notes.

| Directory | Contents |
| --- | --- |
| [mt7915-icap/](mt7915-icap/README.md) | the driver changes — the debugfs ICAP capture knobs, the relay/events plumbing and the kernel VITA 49 UDP sender — as a single patch against pristine mt76, with its rebuild helper |

The consolidated patch applies to the pinned pristine mt76 source. The built
module ships to the board as `mt7915e.vrt.ko` (see
[../init/README.md](../init/README.md)).

## Licence

Driver patches are ISC, keeping MediaTek's copyright on the files they modify;
see [../LICENSE](../LICENSE) and the per-file SPDX headers.
