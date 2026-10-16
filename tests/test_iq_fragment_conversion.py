#!/usr/bin/env python3
"""Exercise the actual driver conversion helper across arbitrary skb boundaries."""
from pathlib import Path
import subprocess, tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'kernel/mt7915-icap/udp-export.inc').read_text()
s=s[s.index('static int mt7915_iq_convert'):s.index('static int mt7915_iq_worker')]
prefix=r'''
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define min(a,b) ((a)<(b)?(a):(b))
struct sk_buff { u8 data[8240]; };
struct skb_seq_state { struct sk_buff *skb; unsigned split; };
static unsigned skb_seq_read(unsigned off, const u8 **p, struct skb_seq_state *st) {
    *p=st->skb->data+44+off;
    return min(8192-off, st->split-off%st->split);
}
static int skb_copy_bits(struct sk_buff *s,unsigned off,void *p,unsigned n) {
    if(off+n>sizeof(s->data)) return -1; memcpy(p,s->data+off,n); return 0;
}
static u32 get_unaligned_le32(const void *p) { u32 n;memcpy(&n,p,4);return n; }
static void put_unaligned_le32(u32 v,void *p) {memcpy(p,&v,4);}
static void put_unaligned_be32(u32 v,void *p) {v=__builtin_bswap32(v);memcpy(p,&v,4);}
static void put_unaligned_be64(u64 v,void *p) {v=__builtin_bswap64(v);memcpy(p,&v,8);}
'''
suffix=r'''
int main(void) {
 struct sk_buff skb; struct skb_seq_state st={.skb=&skb};
 u8 out[8192], expected[8192];
 for(unsigned i=0;i<sizeof(skb.data);i++) skb.data[i]=rand();
 for(int bits=8;bits<=16;bits+=8) {
  unsigned bps=bits/4;
  for(unsigned i=0;i<2048;i++) {
   u32 v=get_unaligned_le32(skb.data+44+i*4);
   if(bits==8) {expected[i*2]=v>>22;expected[i*2+1]=v>>6;}
   else put_unaligned_be32((v<<2)&0xfffcfffc,expected+i*4);
  }
  for(st.split=1;st.split<3000;st.split++) {
   memset(out,0,sizeof(out));
   for(unsigned i=0;i<2048;) {
    unsigned n=min(bits==16?360u:720u,2048-i);
    if(mt7915_iq_convert(&skb,&st,i*4,n,out+i*bps,bits)) return 1;
    i+=n;
   }
   if(memcmp(out,expected,2048*bps)) {fprintf(stderr,"bad split %u bits %d\n",st.split,bits);return 2;}
  }
 }
 puts("fragment conversion: both formats, 2999 fragment sizes each passed");
}
'''
with tempfile.TemporaryDirectory() as td:
    p=Path(td); (p/'test.c').write_text(prefix+s+suffix)
    subprocess.run(['cc','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
