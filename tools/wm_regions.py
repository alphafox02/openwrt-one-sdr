#!/usr/bin/env python3
"""Parse the connac2 WM trailer and map download regions like mt76 does.

Region data starts at file offset ZERO. Descriptors are 40 bytes each, followed
by a 36-byte trailer. The MT7981 image also has 100 bytes between region data
and descriptors; these are not a leading header. Never infer offsets by
subtracting total region lengths from the file size.

Usage: wm_regions.py [va ADDRESS | fo OFFSET]
WM_IMAGE and WM_OUTDIR override the source image and extraction directory.
"""
import os
import struct
import sys
import zlib
from pathlib import Path

WM = os.environ.get("WM_IMAGE", str(Path(__file__).resolve().parents[1] / "vendor/mt7981_wm.bin"))
OUTDIR = os.environ.get("WM_OUTDIR", "/tmp/wm_regions")


def parse(data):
    if len(data) < 36:
        raise ValueError("truncated firmware trailer")
    if zlib.crc32(data[:-4]) != struct.unpack_from("<I", data, len(data)-4)[0]:
        raise ValueError("firmware CRC32 mismatch")
    count = data[-34]
    desc_start = len(data) - 36 - count * 40
    if not count or desc_start < 0:
        raise ValueError("invalid region count")
    result, offset = [], 0
    for i in range(count):
        desc = desc_start + i * 40
        addr, length = struct.unpack_from("<II", data, desc + 16)
        if not length or offset + length > desc_start:
            raise ValueError("region data overlaps descriptors")
        result.append((i, addr, length, offset, offset + length))
        offset += length
    return result


def spans():
    return parse(Path(WM).read_bytes())


def va_of(file_off):
    for _, addr, length, start, end in spans():
        if start <= file_off < end:
            return addr + file_off - start
    return None


def fo_of(va):
    for _, addr, length, start, end in spans():
        if addr <= va < addr + length:
            return start + va - addr
    return None


def extract():
    data = Path(WM).read_bytes()
    regions = parse(data)
    Path(OUTDIR).mkdir(parents=True, exist_ok=True)
    print(f"WM={WM} size={len(data)} data_offset=0")
    print(f"OUTDIR={OUTDIR}")
    for i, addr, length, start, end in regions:
        Path(OUTDIR, f"r{i}_{addr:08x}.bin").write_bytes(data[start:end])
        print(f"{i:2} 0x{addr:08x} 0x{length:06x}  0x{start:06x}..0x{end:06x}")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] in ("va", "fo"):
        v = int(sys.argv[2], 0)
        result = (fo_of if sys.argv[1] == "va" else va_of)(v)
        print(f"0x{result:x}" if result is not None else "unmapped")
    else:
        extract()
