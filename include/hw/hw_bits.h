/* SPDX-License-Identifier: MIT */
/*
 * Bit and field helpers shared by every register header under include/hw/.
 *
 * The headers are included by the Linux module, the bare-metal RISC-V
 * firmware and host tools, so they use no kernel or libc macros.
 */
#ifndef HW_BITS_H
#define HW_BITS_H

#define HW_BIT(n)              (1u << (n))
#define HW_GENMASK(h, l)       ((~0u >> (31 - (h))) & (~0u << (l)))

/* Field value from a register word, and a register word from a field value. */
#define HW_SHIFT(mask)         (__builtin_ctz(mask))
#define HW_GET(mask, word)     (((word) & (mask)) >> HW_SHIFT(mask))
#define HW_PREP(mask, val)     (((unsigned int)(val) << HW_SHIFT(mask)) & (mask))

#endif
