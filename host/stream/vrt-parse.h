/* SPDX-License-Identifier: MIT
 * Bounds-checked parser for this project's timestamp-free Ci8/Ci16/packed14 profiles.
 * Other VRT profiles are rejected, not guessed. Data needs preceding context.
 */
#ifndef OWRT_VRT_PARSE_H
#define OWRT_VRT_PARSE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct vrt_state {
    uint32_t stream_id;
    uint64_t rate_fixed, freq_fixed;
    unsigned bits, packed, next_count;  /* packed: 14-bit link-efficient payload */
    int have_count;
};
struct vrt_packet {
    const uint8_t *samples;
    size_t count;
    int boundary; /* unknown-length discontinuity; never infer missing samples */
};
static inline uint32_t vrt_u32(const uint8_t *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static inline uint64_t vrt_u64(const uint8_t *p) {
    return (uint64_t)vrt_u32(p)<<32 | vrt_u32(p+4);
}
/* Read one MSB-first signed 14-bit field at a bit offset. Reads only the two or
 * three bytes the field spans, so a payload sized to its sample count is not
 * over-read. Used for the packed 14-bit (28-bit/complex) link-efficient profile.
 */
static inline int vrt_get14(const uint8_t *p, size_t bitpos) {
    size_t byte = bitpos >> 3; unsigned sh = bitpos & 7u;
    uint32_t v = ((uint32_t)p[byte] << 8) | p[byte+1];
    unsigned total = 16;
    if (sh + 14u > 16u) { v = (v << 8) | p[byte+2]; total = 24; }
    v = (v >> (total - sh - 14u)) & 0x3fffu;
    return (int)(v ^ 0x2000u) - 0x2000;   /* sign-extend 14 bits */
}
/* Returns 0 for malformed/unsupported, 1 for context, 2 for signal data.
 * Invalid input never changes state. Packet counter wraps at 16: a loss of
 * exactly 16 packets is invisible without an additional source counter.
 */
static inline int vrt_parse(const uint8_t *d, size_t n,
                            struct vrt_state *s, struct vrt_packet *p) {
    uint32_t h, type;
    memset(p, 0, sizeof(*p));
    if (n < 8 || n % 4) return 0;
    h = vrt_u32(d); type = h >> 28;
    if ((h & 0x0ff00000u) || (h & 0xffffu)*4u != n) return 0;
    if (type == 4) {
        struct vrt_state next = *s;
        uint32_t cif, fmt, width;
        /* Exact supported context: RF reference + sample rate + format. */
        if (n != 36) return 0;
        cif = vrt_u32(d+8);
        if ((cif & ~0x80000000u) != ((1u<<27)|(1u<<21)|(1u<<15))) return 0;
        fmt = vrt_u32(d+28); width = (fmt & 0x3fu)+1;
        /* Ci8/Ci16 are processing-efficient (one sample per item, byte-aligned);
         * 14-bit is link-efficient packed (bit 31 set, 28 bits per complex). */
        if (width == 8 || width == 16) {
            if (fmt != ((1u<<29)|((width-1)<<6)|(width-1))) return 0;
            next.packed = 0;
        } else if (width == 14) {
            if (fmt != ((1u<<31)|(1u<<29)|((width-1)<<6)|(width-1))) return 0;
            next.packed = 1;
        } else return 0;
        if (vrt_u32(d+32)) return 0;
        next.stream_id = vrt_u32(d+4);
        next.freq_fixed = vrt_u64(d+12);
        next.rate_fixed = vrt_u64(d+20);
        if (!next.rate_fixed || next.rate_fixed >> 63 || next.freq_fixed >> 63) return 0;
        next.bits = width;
        p->boundary = s->bits && (next.stream_id != s->stream_id ||
            next.bits != s->bits || next.packed != s->packed ||
            next.rate_fixed != s->rate_fixed || next.freq_fixed != s->freq_fixed);
        if (p->boundary) next.have_count = 0;
        *s = next;
        return 1;
    }
    if (type != 1 || !s->bits || vrt_u32(d+4) != s->stream_id || n == 8) return 0;
    /* This timestamp/class-ID-free packed14 profile carries no pad-bit count.
     * Require whole groups of eight complex samples (28 bytes), without tail
     * padding. Otherwise seven samples padded to 32-bit words can look like
     * eight, and the receiver cannot recover the intended sample count. */
    if (s->packed && (n-8) % 28u) return 0;
    p->samples = d+8;
    p->count = s->packed ? ((size_t)(n-8)*8u)/28u : (n-8)/(s->bits/4);
    p->boundary = s->have_count && ((h>>16)&15) != s->next_count;
    s->next_count = (((h>>16)&15)+1)&15;
    s->have_count = 1;
    return 2;
}
#endif
