# tools/ — host-side tooling

Utilities for building, inspecting, and talking to the device.

## nds32 binutils

- **`nds32-as`, `nds32-ld`, `nds32-objcopy`, `nds32-objdump`** — GNU binutils
  for the `nds32le-elf` target. Used to assemble/inspect on-chip code
  ([../firmware/](../firmware/)) and to disassemble the stock WM blobs
  ([../vendor/](../vendor/)). `ldscripts/` holds link scripts for placing
  `rom_patch` code at the exec gap.

## Firmware container / RE

- **`wm_regions.py`** — parser for the MediaTek RAM container (the WM/WA image
  format: region data from file offset 0, 40-byte descriptors, per-region
  checksums, trailing zlib CRC32). Gives `va_of(file_off)` / `fo_of(va)` /
  `spans()` so disassembly addresses line up with the file. **Per-region
  checksums matter**: patching requires updating both the per-region checksum
  and the whole-file CRC (missing the per-region ones is what caused earlier
  "firmware init timeout" failures).
- **`wm_pin_gp.py`** — pins the nds32 `gp` (global pointer, `0x02216800`) for
  correct disassembly of gp-relative accesses.

## Device

- **`console.py`** — serial console helper (`/dev/ttyACM0 @115200`, root, no
  password) for when SSH/IP is unavailable (e.g. after a reboot changes DHCP).
- **`icap-device.sh`**, **`one.sh`** — device-side helper snippets.

## icap-device.sh

Router-side shell helpers (source it on the device): `rr`/`wr` register access,
`ce`/`ext` MCU command builders over the `icap_prime` knob, and the
`native_capture` / `native_retire` for the MT7981 native EXT 0x56 capture.

## live/

- **`iqloop.sh`** — router-side gapped capture loop: native capture → retire →
  read the 64 KiB `icap_window`, emitting `IQ01` frames on stdout.

## webui/

A local, offline **live (gapped) spectrum viewer**: spectrum, waterfall and
constellation from real captures, ~6 frames/s over an SSH pipe. See
[webui/README.md](webui/README.md). The final streaming UI is the reused W1700K
stack in [../host/](../host/); this is the bring-up viewer.
