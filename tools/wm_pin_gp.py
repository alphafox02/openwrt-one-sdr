#!/usr/bin/env python3
"""Constrain the MT7981 WM's nds32 global pointer ($gp = _SDA_BASE_).

$gp is NEVER written in any of the 11 WM regions and the rom_patch only
saves/restores it (push25/pop25) -- it is inherited from the on-chip mask
ROM (matches W1700K's "takes the ROM's gp"). So it can only be constrained
statically, not read, from these blobs.

Method: every gp-relative access [gp+off] must resolve into mapped memory.
Sweeping candidate $gp and scoring the fraction of the 4352 gp-relative
uses that land in a mapped region pins $gp to a REGION (not an exact value):
only the 0xf0000000 SYSRAM chain scores 100% -- the 0x022xxxxx and
0x00400000 data areas do not. The iCap context [gp+0x24fd8] therefore lives
near 0xf0034xxx (region r7, 0xf0025800). Exact low bits need a live read of
the running WM MCU or the mask ROM.

Input: directory of `tools/nds32-objdump` disassembly files dis_r*.txt
(one per region, produced with --adjust-vma=<load_addr>). Pass the dir as
argv[1] (default: $WM_OUTDIR or /tmp/wm_regions).
"""
import re, glob, collections, os, sys

DISDIR = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("WM_OUTDIR", "/tmp/wm_regions")
TABLE = [  # (load_addr, len) -- mapped spans, from wm_regions.py
    (0x0220a800, 0x1f000), (0x0231dc00, 0x32400), (0x00414800, 0x3800),
    (0xe003b000, 0x61400), (0xe009c400, 0x73800), (0x02229800, 0x1c000),
    (0x00411600, 0x3200),  (0xf0025800, 0x3e800),  (0xf0000000, 0x25800),
    (0xf0064000, 0x32000), (0xf0096000, 0x16d40),
]
SPANS = [(a, a + l) for a, l in TABLE]

re_gprel = re.compile(r'\b[ls][bhw][a-z]*\.gp\s+\$r\d+, \[ \+ #(-?0x[0-9a-f]+)\]')
re_addigp = re.compile(r'\baddi\.gp\s+\$r\d+, #(-?0x[0-9a-f]+)')


def mapped(x):
    x &= 0xffffffff
    return any(s <= x < e for s, e in SPANS)


def main():
    offs = collections.Counter()
    files = sorted(glob.glob(os.path.join(DISDIR, "dis_r*.txt")))
    if not files:
        sys.exit(f"no dis_r*.txt in {DISDIR}; disassemble regions there first")
    for fn in files:
        for ln in open(fn):
            for m in re_gprel.finditer(ln):
                offs[int(m.group(1), 16)] += 1
            m = re_addigp.search(ln)
            if m:
                offs[int(m.group(1), 16)] += 1
    tot = sum(offs.values())
    print(f"gp-relative uses: {tot} ({len(offs)} distinct offsets, "
          f"range {min(offs):#x}..{max(offs):#x})")
    best = []
    for base, end in [(0xf0000000, 0xf0010000), (0x0220a800, 0x0221a800),
                      (0x02229800, 0x02239800), (0x00400000, 0x00412000)]:
        for gp in range(base, end, 4):
            s = sum(c for o, c in offs.items() if mapped(gp + o))
            best.append((s, gp))
    best.sort(reverse=True)
    print("\n top candidate $gp (mapped-uses / total):")
    for s, gp in best[:8]:
        print(f"  gp=0x{gp:08x}  {s}/{tot} ({100*s/tot:.1f}%)  "
              f"[gp+0x24fd8]=0x{(gp+0x24fd8)&0xffffffff:08x}")


if __name__ == "__main__":
    main()
