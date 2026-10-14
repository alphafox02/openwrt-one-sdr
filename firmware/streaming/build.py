#!/usr/bin/env python3
"""Build the bounded MT7981 DMA experiment; verify both PDA and container CRC."""
from pathlib import Path
import hashlib, struct, subprocess, sys, zlib
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
from wm_regions import parse
out = Path(sys.argv[1] if len(sys.argv)>1 else '/tmp/owrt-window-dma')
pool = '--pool' in sys.argv[2:]
out.mkdir(parents=True, exist_ok=True)
b = bytearray((ROOT/'vendor/mt7981_wm.bin').read_bytes())
assert hashlib.md5(b).hexdigest() == 'e11d45ca356de61d17b1024097ef8137'
regions = parse(b)
def offset(va):
    return next(start+va-addr for _,addr,n,start,end in regions if addr<=va<addr+n)
def run(tool,*args):
    subprocess.run([str(ROOT/'tools'/('nds32-'+tool)),*map(str,args)],check=True)
run('as','-EL','-o',out/'dma.o',ROOT/'firmware/streaming/window_dma.S')
run('ld','-L',ROOT/'tools/ldscripts','--no-relax','-Ttext=0xe009b000',
    '--defsym=command_resume=0xe006cf6c','--defsym=command_null=0xe006cfa2',
    '--defsym=packet_alloc=0xe003d500','--defsym=packet_free=0xe003d57a',
    '--defsym=reclaim=0x0081b5b2','-e','command','-o',out/'dma.elf',out/'dma.o')
run('objcopy','-O','binary',out/'dma.elf',out/'dma.bin')
code=(out/'dma.bin').read_bytes()
assert len(code)<0x800
start=offset(0xe009b000)
assert not any(b[start:start+0xc00])
b[start:start+len(code)]=code
off=offset(0xe006cf68)
assert b[off:off+4] == bytes.fromhex('fc20c01c')
b[off:off+4]=struct.pack('>I',0x48000000|(((0xe009b000-0xe006cf68)//2)&0xffffff))
if pool:
    # Keep normal radio boot, but use the stock mode-2 pool partition path:
    # layout(PLE=1,PSE=2,ICAP=0), ICAP=256 banks, native profile 5.
    # Original mov55 r7,r0 -> movi55 r7,2 inside hal UMAC init wrapper.
    off=offset(0xe00597b0)
    assert b[off:off+2] == bytes.fromhex('80e0')
    b[off:off+2] = bytes.fromhex('84e2')
def pda(data):
    state=0
    for word, in struct.iter_unpack('<I', data):
        state=(state>>1)^(((state&278008081).bit_count()&1)<<31)^word
    return state
for _,addr,n,start,end in regions:
    state=pda(b[start:end])
    tail=struct.unpack_from('<I',b,end-4)[0]
    struct.pack_into('<I',b,end-4,tail^state)
    assert pda(b[start:end])==0
struct.pack_into('<I',b,len(b)-4,zlib.crc32(b[:-4]))
(out/'wm-window-dma.bin').write_bytes(b)
print('code bytes',len(code),'image md5',hashlib.md5(b).hexdigest())
