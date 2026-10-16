/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdlib.h>
#include "../host/stream/vrt-parse.h"
static void be32(uint8_t *p, uint32_t v) { for (int i=3;i>=0;i--) { p[i]=v; v>>=8; } }
static void be64(uint8_t *p, uint64_t v) { be32(p,v>>32); be32(p+4,v); }
int main(void) {
    uint8_t ctx[36]={0}, data[1032]={0};
    struct vrt_state s={0}, before;
    struct vrt_packet p;
    be32(ctx,0x40000009); be32(ctx+4,1); be32(ctx+8,(1u<<27)|(1u<<21)|(1u<<15));
    be64(ctx+12,2437000000ull<<20); be64(ctx+20,40000000ull<<20);
    be32(ctx+28,(1u<<29)|(15u<<6)|15);
    be32(data,0x10000001); be32(data+4,1);
    assert(!vrt_parse(data,8,&s,&p)); /* original underflow crash */
    for (size_t n=0;n<36;n++) assert(!vrt_parse(ctx,n,&s,&p));
    assert(vrt_parse(ctx,36,&s,&p)==1 && s.bits==16);
    before=s; ctx[28]|=0x80;
    assert(!vrt_parse(ctx,36,&s,&p) && !memcmp(&s,&before,sizeof(s)));
    ctx[28]&=0x7f;
    be32(data,0x100f0102);
    assert(vrt_parse(data,sizeof(data),&s,&p)==2 && p.count==256 && !p.boundary);
    be32(data,0x10000102);
    assert(vrt_parse(data,sizeof(data),&s,&p)==2 && !p.boundary); /* 15 -> 0 */
    be32(data,0x10020102);
    assert(vrt_parse(data,sizeof(data),&s,&p)==2 && p.boundary); /* skip 1 */
    be64(ctx+20,80000000ull<<20);
    assert(vrt_parse(ctx,36,&s,&p)==1 && p.boundary && !s.have_count);
    be32(ctx+28,(1u<<29)|(7u<<6)|7);
    assert(vrt_parse(ctx,36,&s,&p)==1 && p.boundary && s.bits==8);
    assert(vrt_parse(data,sizeof(data),&s,&p)==2 && p.count==512);
    /* Exercise arbitrary lengths and headers at exact allocation boundaries. */
    srand(49);
    for (int i=0;i<100000;i++) {
        size_t n=rand()%2049;
        uint8_t *b=malloc(n ? n : 1);
        for (size_t j=0;j<n;j++) b[j]=(uint8_t)rand();
        vrt_parse(b,n,&s,&p);
        free(b);
    }
    return 0;
}
