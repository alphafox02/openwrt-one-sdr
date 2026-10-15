#!/usr/bin/env python3
"""Validate the bounded eight-packet MT7981 DMA experiment's relay dump."""
import argparse
from pathlib import Path
import struct
import hashlib

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('events', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--reference', type=Path)
a = p.parse_args()
b = a.events.read_bytes()
pos = 0
parts = []
while pos < len(b):
    if pos + 4 > len(b):
        p.error('truncated relay record')
    size, = struct.unpack_from('<I', b, pos)
    rec = b[pos+4:pos+4+size]
    if size != 8252 or len(rec) != size:
        p.error(f'invalid record length {size} at {pos}')
    # 12-byte fw-monitor header, then 44-byte raw DMA event header.
    if struct.unpack_from('<I', rec)[0] != 0x44e98caf:
        p.error('unexpected relay wrapper magic')
    if rec[12+28] != 0xed or rec[12+32] != 0xf1:
        p.error('unexpected MCU event type')
    seq, address = struct.unpack_from('<II', rec, 12+36)
    if seq != len(parts) or address != 0x400000 + seq*8192:
        p.error(f'wrong sequence/source: {seq}, {address:#x}')
    parts.append(rec[56:56+8192])  # exclude four-byte DMA trailer
    pos += 4 + size
if len(parts) != 8:
    p.error(f'expected eight packets, received {len(parts)}')
raw = b''.join(parts)
if a.reference and raw != a.reference.read_bytes():
    p.error('DMA payload differs from reference SRAM capture')
a.output.write_bytes(raw)
print(f'{len(parts)} packets, {len(raw)} bytes, sha256 {hashlib.sha256(raw).hexdigest()}')
if a.reference:
    print('PASS: every byte matches the independently read SRAM capture')
