#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Patch a *local* stock MT7981 Wi-Fi firmware blob into the SDR firmware.

This injects this project's own nds32 DMA producer (embedded below, pre-assembled
MIT code) into a copy of the MediaTek WM firmware that is already present on the
user's own board. It performs no network access and ships no MediaTek code: the
output is derived entirely from the stock blob the caller supplies. The patched
firmware is therefore produced locally and is never redistributed by this project.

Usage:
    apply-producer.py <stock mt7981_wm.bin> <output wm-sdr.bin>

The input must be the stock OpenWrt One WM firmware (md5 e11d45ca356de61d17b1024097ef8137).
"""
import base64, hashlib, struct, sys, zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
# Find wm_regions.py whether run from the repo (tools/) or a flat release bundle.
sys.path[:0] = [str(HERE), str(ROOT/'tools')]
from wm_regions import parse

STOCK_MD5 = 'e11d45ca356de61d17b1024097ef8137'

# Pre-assembled nds32 DMA producer (firmware/streaming/window_dma.S), MIT.
PRODUCER_B64 = (
"""/CBOAwAIRv4AbFj3j6JKADwABHAAAwRjgBBaYAMKWmAEDVpgBQZaYAYESP6PoQQDgBFJAAAo/KBG
SAAjWEIAkEQAJPMUAgAARhAAMFgQgABQEP//ThP//kYABABYAAAARiAAAVghAABkAAEhUAAAIFAh
f/9OI//6ZAAACEkAAAP8oPxmFA+ACxRvgARG7gCbWOcIAEQAAAEUBwAARAAAABQHAAEUD4AIFA+A
CRQPgAZQJwBgRDAAEBwBAAFQMf//TjP//BQHABBQBwBAZAABIWQAAAgV1wAERAAAUEn9EhIUD4AA
TgICEwSgAAMUpwAFZAIAAhQPgAFkAgBDZAAACEZlQABYYwMABHMAAASDAAIEAwADTARBowQDAAFE
EAAETADBnUQAAABEEAAARiAIG1ghBbLdIkYAADdYAA0sQL6AAAQFgABMA8GKBAWADRQPgAJGCJAg
WAAAaAQAAAAUD4ADRAAAAEQQAAuAShwBAAFQEP//ThP//EYDgAJYAAAsFAUAAEYDgAJYAAAUFAUA
BkQAAO0UBQAHRAAA8RQFAAhGkAQAWJSAAAQPgARaAAUEWggGBkaeAgBYlIAARMAAAEQgAAgUL4AK
WggGQAQvgAtOIwAERCAAgBQvgApEIAAAFC+ABRQvgAZGSIAJWEIAKEQgAAEUIgAARkgAI1hCAJBE
IAAAFCIAAEQgBAAUIgAARCAAABQiAAJGIAAfWCEP/BQiAANEICQBFCIAAEYokCBYIQBoBCEAABQv
gAMUL4AHRCAkAxQiAAAED4AEWggGGkYQAPRYEIJASQABYAQvgAhOIwC2XCAIAE4iAAxQEP//ThP/
9EQgAAYUL4AISAAAqEYIkCBYAABoBAAAABQPgAsUxQAJFJUACkA0EAhAM4wAFKGAABSRgAJEAAAA
FAGAA0YAAsJYAAAAFAGAAVCEAAFUhAADQDQQCEAzjAAUoYAARAAAABQBgAIUAYADRgQAQFgAAAAU
AYABZAAACFCEAAFUhAADFIMAAkYQAPRYEIJABA+ABFoIBgRJAAEIBAMAA0wEQCpkAAAIUCR//1Qh
AANAIRAIQCOIAGQBAQFkAAAIBAEAAU4EABJQJH/+VCEAA0AhEAhAI4gAZAEBAWQAAAgEAQABTgUA
EAQHACZQAAABFAcAJlAQ//9OE//MRAAABEgAAJhGKJAgWCEAaAQhAAAEP4ALQCEMAU4lAAoENwAY
QDGIBk4yAAQUJwAYUMYAARTHAAFQlKAARg4CgFgAAABMkEAGRp4CAFiUgAAED4AITgMABgQPgApM
wH9ABA+ABFoIBgpGGAAjWBCAkEQABAAUAIAAgCdEIAAERgwAAFgAAABEMAAAFACAARQwgAJQEIAQ
UCF//04j//gUhYALBA+AAkAWBAhAAAQAFAWADRQFgA5GGJAgWBCAaAQAgAAEL4ADQAAIARQHAAIE
D4AGFAcABgQPgAkUBwAHBA+ACE4DAAhEAAACSAAABEQAAAMUBwAAZAcBIVAHACBkAAEhUAcAYGQA
ASFQBwCAZAABIWQAAAgED4ABZAIAA2QAAAkED4AARBAAAUn9EHJEAAAA/OYEL4AITiMABkQwAARJ
AACwRigAI1ghAJBEAAQAFAEAAEQAAAQUBwAABA+ABhQHAAYED4AJFAcAB2QHASFQBwAgZAABIVAH
AGBkAAEhUAcAgGQAASFkAAAIBA+AAWQCAANkAAAJRAAAAPzmRAAABxQHAABEAAAA/OYEL4AITiMA
qVAnAEBkAQEBZAAACAQnABBOIgAGRDAACUgAAGpGKJAgWCEAaAQhAAAEP4AHFC+AB0AxDAEUNwAJ
TjQACFAxgCBONAAaSAAADgQHABlAAAwGTgIABBQ3ABlcMYH0TjMADBQnAAoENwAJFDcAC0QwAAhI
AAA+RigAI1ghALQEIQAABD+ABRQ3AB4UJwAfFC+ABUAhDAFGMAAfWDGP/0AhDAIUJwAgBDcAGkAx
iAZOMgAEFCcAGgQPgAZAAAgAFA+ABkAmLAhAAAgBBD+ACUAhgAZOIgAEFA+ACUYwAB9YMYAAQCAM
Bk4jADREMAAFFD+ACBQ3ABsED4AGQCYsCEAACAEUBwAcFMcAHQQnAAkUJwAhBCMAAxQnACIUhwAj
UCR//lQhAANAIRAIQCOIAAQhAAEUJwAkUCR//1QhAANAIRAIQCOIAAQhAAEUJwAl3Z4ED4AGQCYs
CEAACAHdng=="""
)
CODE = base64.b64decode(PRODUCER_B64)

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    inp, outp = sys.argv[1], sys.argv[2]
    b = bytearray(Path(inp).read_bytes())
    got = hashlib.md5(b).hexdigest()
    if got != STOCK_MD5:
        sys.exit(f"refusing: {inp} md5 {got} is not the stock WM firmware "
                 f"({STOCK_MD5}). Point this at the unmodified blob from your "
                 f"board's /lib/firmware/mediatek/mt7981_wm.bin.")
    regions = parse(b)
    def offset(va):
        return next(start+va-addr for _,addr,n,start,end in regions if addr<=va<addr+n)
    assert len(CODE) < 0x800
    start = offset(0xe009b000)
    assert not any(b[start:start+0xc00]), "producer landing zone not clear"
    b[start:start+len(CODE)] = CODE
    off = offset(0xe006cf68)
    assert b[off:off+4] == bytes.fromhex('fc20c01c'), "unexpected branch site"
    b[off:off+4] = struct.pack('>I', 0x48000000|(((0xe009b000-0xe006cf68)//2)&0xffffff))
    # Pool partition path: stock mode-2 ICAP pool, native profile 5.
    off = offset(0xe00597b0)
    assert b[off:off+2] == bytes.fromhex('80e0'), "unexpected pool site"
    b[off:off+2] = bytes.fromhex('84e2')
    # Recompute the per-region PDA tail words, then the container CRC32.
    def pda(data):
        state = 0
        for word, in struct.iter_unpack('<I', data):
            state = (state>>1)^(((state&278008081).bit_count()&1)<<31)^word
        return state
    for _,addr,n,start,end in regions:
        state = pda(b[start:end])
        tail = struct.unpack_from('<I', b, end-4)[0]
        struct.pack_into('<I', b, end-4, tail^state)
        assert pda(b[start:end]) == 0
    struct.pack_into('<I', b, len(b)-4, zlib.crc32(b[:-4]))
    Path(outp).write_bytes(b)
    print(f"wrote {outp}  ({len(CODE)} producer bytes injected)  "
          f"md5 {hashlib.md5(b).hexdigest()}")

if __name__ == '__main__':
    main()
