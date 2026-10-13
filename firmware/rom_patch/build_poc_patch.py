#!/usr/bin/env python3
"""Build the POC rom_patch: redirect an existing MediaTek hook through a custom
nds32 handler placed in the patch's executable padding gap.

Proven on hardware 2026-10-13: the handler runs on the MT7981 WiFi MCU (wrote a
unique marker to patch RAM, read back from the host via regidx) while the device
boots cleanly, because the handler preserves the original hook's behavior.

Constraints discovered (the hard way):
  * The handler MUST live in the ORIGINAL executable body. Bytes appended past
    the body end (>= 0x009025c0) load fine but are NOT executable -> jumping
    there faults. Use the zero-padding gap at VA 0x00902300..0x009024c0.
  * Patch RAM IS writable at runtime (marker to 0x00900204 works). SYSRAM
    0x00464000 is NOT MCU-writable (faults). WM SYSRAM 0x00400000 is used.
  * Interpose on an EXISTING hook (redirect its TO[i]) -- those ROM functions
    are proven safe + executed. Hooking an arbitrary ROM fn (e.g. 0x804d14)
    crashed. FROM must be a valid in-ROM address (0x00838000, past ROM, faults).
  * Use `jr` (register-indirect); gas mis-encodes `j <absolute-literal>`.

Usage:
  nds32-as  -o h.o  hook_marker.s
  nds32-ld  -Ttext=0x00902300 -e _start -o h.elf h.o   # ld needs its ldscripts/
  nds32-objcopy -O binary h.elf hook_marker.bin
  python3 build_poc_patch.py <pristine_rom_patch.bin> hook_marker.bin out.bin
Then on the device (via fwguard, docs/../device/fwguard): place out.bin as
/root/rom_patch.test, `touch /root/fwguard.arm`, `reboot` (NOT rmmod/modprobe).
Read the marker: write 0x00900204 to regidx, read regval (== 0xc0decafe).
"""
import struct, sys, hashlib

PRISTINE, HANDLER, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
HANDLER_VA = 0x00902300   # executable zero-padding gap in the patch body
# redirect TO[slot]; slot 0 = POC marker (FROM 0x008013ae), slot 1 = capture
# handler (FROM 0x00829d3a -> orig 0x0090122c). Override with argv[4].
REDIRECT_SLOT = int(sys.argv[4]) if len(sys.argv) > 4 else 0

d = bytearray(open(PRISTINE, "rb").read())
HDR, BODY = 96, 0xa0
typ, offs, size = struct.unpack_from(">III", d, HDR)
addr, blen, key, align = struct.unpack_from(">IIII", d, HDR + 12)
assert offs == BODY and addr == 0x00900000, (hex(offs), hex(addr))

h = open(HANDLER, "rb").read()
h += b"\x00" * ((-len(h)) % 4)
hoff = BODY + (HANDLER_VA - 0x00900000)
assert all(b == 0 for b in d[hoff:hoff + len(h)]), "handler target is not padding!"
d[hoff:hoff + len(h)] = h                                   # place handler in the gap
old = struct.unpack_from("<I", d, BODY + 0x108 + REDIRECT_SLOT * 4)[0]
struct.pack_into("<I", d, BODY + 0x108 + REDIRECT_SLOT * 4, HANDLER_VA)  # TO[slot] -> handler
# length unchanged (no append); checksum not enforced by the ROM.
open(OUT, "wb").write(d)
print(f"TO[{REDIRECT_SLOT}]: 0x{old:08x} -> 0x{HANDLER_VA:08x}; body len unchanged 0x{blen:x}")
print(f"{OUT}: {len(d)} bytes, md5 {hashlib.md5(bytes(d)).hexdigest()}")
