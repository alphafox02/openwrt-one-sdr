# kernel/mt7915-icap

Changes to OpenWrt's mt76 (mt7915) driver that expose the MT7981 ICAP capture
engine and stream the samples out as VITA 49 over UDP.

| File | Role |
| --- | --- |
| `complete-local.patch` | the driver changes as a single patch against pristine mt76 `c246fa545119abe5097682316700f13c91399042`: the debugfs ICAP capture knobs, the relay/event plumbing, the VITA 49 UDP exporter, and the experimental BSS gate |
| `rebuild.py` | re-run the cached compile/link commands in a prepared OpenWrt build tree, recompiling every object before linking |
| `sdrdriverguard` | boot guard that restores the stock module if a trial driver fails to come up |

## Build

```sh
# against a pristine mt76 source tree:
patch -p1 < complete-local.patch

# or, in a prepared OpenWrt build tree, rebuild the changed objects and relink:
python3 kernel/mt7915-icap/rebuild.py
```

The patch changes `struct mt7915_dev`, so every mt7915 object must be rebuilt,
not only the edited ones — `rebuild.py` does this. The built module ships to the
board as `mt7915e.vrt.ko` (see [../../init/README.md](../../init/README.md)).

The exporter intercepts only the 8240-byte `f1` IQ events; the `iq_udp` debugfs
knob configures a bounded kernel worker that targets the directly connected
host. Keep the MCU receive-ring size 512 and buffer size 2048; larger rings did
not stay up in testing.

## Licence

Driver changes are ISC, keeping MediaTek's copyright on the files they modify;
see [../../LICENSE](../../LICENSE) and the per-file SPDX headers.
