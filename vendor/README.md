# vendor/ — stock MediaTek firmware blobs

Proprietary MediaTek Wi-Fi firmware for the MT7981, as shipped in OpenWrt /
vendor trees. Kept here for **analysis** (disassembly with the nds32 tools) and
for on-device use with the hardware they came with.

- `mt7981_wm.bin` — the WiFi MCU ("WM") firmware (nds32). The ICAP/capture and
  RF code we reverse-engineer lives here.
- `mt7981_wa.bin` — the WiFi offload ("WA") firmware.
- `mt7981_rom_patch.bin` — the stock ROM patch (has the exec gap we load custom
  code into).

## License

These are **MediaTek's proprietary firmware**, not ours. They are **not
relicensed** by this repo and carry no CEMAXECUTER copyright. They are included
only for interoperability/analysis with the device that ships them. If you
redistribute this repo, treat these as you would any vendor firmware blob. See
[../LICENSE](../LICENSE).

Use `../tools/wm_regions.py` to parse the container and
`../tools/nds32-objdump` (with `gp=0x02216800`) to disassemble.
