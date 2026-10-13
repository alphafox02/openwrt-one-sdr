#!/usr/bin/env python3
"""Build a behavior-preserving trace image for the pinned stock WM."""
from pathlib import Path
import hashlib, struct, subprocess, sys, zlib
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from wm_regions import parse
out = Path(sys.argv[1] if len(sys.argv)>1 else '/tmp/owrt-trace')
out.mkdir(parents=True, exist_ok=True)
b = bytearray((ROOT/'vendor/mt7981_wm.bin').read_bytes())
assert hashlib.md5(b).hexdigest() == 'e11d45ca356de61d17b1024097ef8137'
regions = parse(b)
def offset(va):
    return next(start+va-addr for _,addr,n,start,end in regions if addr<=va<addr+n)
def run(tool,*args):
    subprocess.run([str(ROOT/'tools'/('nds32-'+tool)),*map(str,args)],check=True)
run('as','-EL','-o',out/'trace.o',ROOT/'firmware/diagnostic/trace.S')
run('ld','-L',ROOT/'tools/ldscripts','--no-relax','-Ttext=0xe009b000',
    '--defsym=stock_status=0xe006cfbe','--defsym=periodic_resume=0xe006cfcc',
    '--defsym=command_resume=0xe006cf6c','--defsym=command_null=0xe006cfa2','-e','periodic','-o',out/'trace.elf',out/'trace.o')
run('objcopy','-O','binary',out/'trace.elf',out/'trace.bin')
code=(out/'trace.bin').read_bytes()
assert len(code)<0x800
start=offset(0xe009b000)
assert not any(b[start:start+0xc00])
b[start:start+len(code)]=code
# nds32 instruction bytes are big-endian even in a little-endian data image.
def hook(va,target,expected):
    off=offset(va)
    assert b[off:off+len(expected)]==expected,(hex(va),b[off:off+len(expected)].hex())
    delta=target-va
    assert delta%2==0 and -(1<<24)<=delta<(1<<24)
    ins=struct.pack('>I',0x48000000|((delta//2)&0xffffff))
    b[off:off+len(expected)]=ins+b'\x92\x00'*((len(expected)-4)//2)
hook(0xe006cfc6,0xe009b000,bytes.fromhex('fc0049fffffb'))
# Symbol follows periodic; obtain from objdump so source edits stay safe.
symbols=subprocess.check_output([str(ROOT/'tools/nds32-objdump'),'-t',str(out/'trace.elf')],text=True)
cmd=int(next(line.split()[0] for line in symbols.splitlines() if line.endswith(' command')),16)
hook(0xe006cf68,cmd,bytes.fromhex('fc20c01c'))
# MediaTek PDA checksum: linear feedback over little-endian words.
# Algorithm documented by BirriaSDR (Luigi C., MIT).
for _,addr,n,start,end in regions:
    state=0
    for word, in struct.iter_unpack('<I', b[start:end]):
        state=(state>>1)^(((state&278008081).bit_count()&1)<<31)^word
    tail=struct.unpack_from('<I',b,end-4)[0]
    struct.pack_into('<I',b,end-4,tail^state)
struct.pack_into('<I',b,len(b)-4,zlib.crc32(b[:-4]))
(out/'wm-trace.bin').write_bytes(b)
print(out/'wm-trace.bin')
