/* SPDX-License-Identifier: MIT
 * Round-trips the packed 14-bit (link-efficient, 28 bits/complex) VITA profile
 * through the shared parser: a context packet selects it, a data packet carries
 * known signed 14-bit I/Q, and vrt_get14 must recover every value exactly.
 */
#include "vrt-parse.h"
#include <stdio.h>
#include <string.h>

static void wr32(uint8_t *p, uint32_t v) {
    p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}
/* MSB-first 14-bit writer, the inverse of vrt_get14. */
static void put14(uint8_t *buf, size_t bitpos, int val) {
    unsigned v = (unsigned)val & 0x3fffu;
    for (int b = 13; b >= 0; b--) {
        size_t bp = bitpos + (size_t)(13 - b);
        if ((v >> b) & 1u) buf[bp>>3] |= (uint8_t)(0x80u >> (bp & 7u));
    }
}

int main(void) {
    const int iq[][2] = {
        {0,0}, {1,-1}, {8191,-8192}, {-8192,8191}, {1234,-4321},
        {-1,1}, {4096,-4096}, {-2731,2730}, {13,-14}, {8190,-8191},
        {7,-7}, {127,-128}, {256,-256}, {1023,-1024}, {777,-333}, {-8192,0},
    };
    const int n = (int)(sizeof(iq)/sizeof(iq[0]));

    /* context: Ci-packed-14, link-efficient (bit31), complex (bit29) */
    uint8_t ctx[36]; memset(ctx, 0, sizeof(ctx));
    wr32(ctx, (4u<<28) | 9u);              /* type 4, 9 words */
    wr32(ctx+4, 0x0A0B0C0D);               /* stream id */
    wr32(ctx+8, (1u<<27)|(1u<<21)|(1u<<15));
    /* freq = 2437 MHz, rate = 40 MHz, both Hz * 2^20 */
    uint64_t freq = (uint64_t)2437000000ull << 20, rate = (uint64_t)40000000ull << 20;
    wr32(ctx+12, (uint32_t)(freq>>32)); wr32(ctx+16, (uint32_t)freq);
    wr32(ctx+20, (uint32_t)(rate>>32)); wr32(ctx+24, (uint32_t)rate);
    wr32(ctx+28, (1u<<31)|(1u<<29)|(13u<<6)|13u);  /* packed 14-bit */
    wr32(ctx+32, 0);

    /* data: 8-byte prologue + packed payload, padded up to a word */
    size_t payload_bits = (size_t)n * 28;
    size_t payload_bytes = (payload_bits + 7) / 8;
    size_t data_words = (8 + payload_bytes + 3) / 4;
    uint8_t data[512]; memset(data, 0, sizeof(data));
    wr32(data, (1u<<28) | (uint32_t)data_words);   /* type 1, count 0 */
    wr32(data+4, 0x0A0B0C0D);
    for (int k = 0; k < n; k++) {
        put14(data+8, (size_t)k*28,      iq[k][0]);
        put14(data+8, (size_t)k*28 + 14, iq[k][1]);
    }

    struct vrt_state st; memset(&st, 0, sizeof(st));
    struct vrt_packet pk;
    if (vrt_parse(ctx, 36, &st, &pk) != 1) { puts("FAIL: context rejected"); return 1; }
    if (!st.packed || st.bits != 14) { puts("FAIL: packed/bits not set"); return 1; }
    if (vrt_parse(data, data_words*4, &st, &pk) != 2) { puts("FAIL: data rejected"); return 1; }
    if ((int)pk.count != n) { printf("FAIL: count %zu != %d\n", pk.count, n); return 1; }
    for (int k = 0; k < n; k++) {
        int i = vrt_get14(pk.samples, (size_t)k*28);
        int q = vrt_get14(pk.samples, (size_t)k*28 + 14);
        if (i != iq[k][0] || q != iq[k][1]) {
            printf("FAIL: sample %d got (%d,%d) want (%d,%d)\n", k, i, q, iq[k][0], iq[k][1]);
            return 1;
        }
    }
    /* A padded ten-sample packet has 36 payload bytes, outside this exact
     * no-tail-padding profile. Rejection must not advance the counter. */
    unsigned next = st.next_count;
    wr32(data, (1u<<28) | 11u);
    if (vrt_parse(data, 44, &st, &pk) != 0 || st.next_count != next) return 1;
    puts("PASS: packed 14-bit context + data round-trip, all samples exact");
    return 0;
}
