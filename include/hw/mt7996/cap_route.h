/* SPDX-License-Identifier: MIT */
/*
 * MT7996 capture route block: decides which SRAM banks the ICAP writers
 * (icap.h) may write, which instance owns the 16 KiB SYSRAM capture window,
 * and whether ICAP0 writes the UMAC pool instead. No name exists for this
 * block in any firmware, ROM table or host source; register names here are
 * descriptive.
 *
 * ADDRESS SPACE: WF bus 0x88009000..0x8800902c, as seen by the WM. Not in a
 * fixed host BAR window; the host drives it through firmware REG_ACCESS
 * (UNI 0x0d, mcu_cmd.h). RESET_MT clears every word to 0.       [measured]
 *
 * Window landing (ICAPn -> SYSRAM 0x464000..0x467fff) needs all of [measured]:
 *   +0x04[31:24] granting the eight window banks,
 *   +0x0c[5:3]   = the owning instance,
 *   +0x24[25]    = 1 (upper 8 KiB; without it only beats 0..511 land),
 *   the instance's write-path enable (+0x28[10] ICAP0, +0x28[21] ICAP1,
 *   +0x2c[10] ICAP2), and +0x28[0] = 0.
 * Pool landing (ICAP0 only): +0x28[0] = 1 alone; no grant bit needed. ICAP1
 * and ICAP2 never write the pool with any settable bit.           [measured]
 *
 * HAZARD: never drive +0x00, +0x04[23:0] or +0x08. These grant SRAM banks
 *   that the MCU firmware is using: the MCU stops answering until RESET_MT
 *   and a firmware reload (no power cycle needed). +0x00 = all-ones also
 *   zeroes every MCU event descriptor; with the route words 0, +0x00 bit 8
 *   alone breaks the event path. Which bits hang depends on the firmware's
 *   SYSRAM partition (the masks below), so treat the whole of these words as
 *   forbidden unless the MCU image is known to keep out of those banks.
 * HAZARD: while a window bank is granted (+0x04[24+k]), host reads of that
 *   bank return a frozen value (the last 64-bit word fetched) and host writes
 *   are dropped. Release the grant (and clear ICAP EN, icap.h) before reading.
 * HAZARD: the firmware's route "type 0" (the pool-oriented variant) ORs
 *   +0x00 |= 0x30000 and +0x04 |= 0xffffff; issued on the stock SYSRAM
 *   layout it kills the firmware command path. Write the type-0 words below
 *   without those two.
 *
 * Confidence tags: [measured] verified on hardware, [static] read from
 * firmware code, [inferred].
 */
#ifndef HW_MT7996_CAP_ROUTE_H
#define HW_MT7996_CAP_ROUTE_H

#include "../hw_bits.h"

#define MT7996_CAP_ROUTE_BASE		0x88009000u
#define MT7996_CAP_ROUTE_SIZE		0x30u
#define MT7996_CAP_ROUTE(ofs)		(MT7996_CAP_ROUTE_BASE + (unsigned int)(ofs))

/* ------------------------------------------------------------------------ */
/* +0x00 GRANT_A: non-window bank grants. Reset 0. DO NOT WRITE.            */
/* ------------------------------------------------------------------------ */
#define MT7996_CAP_ROUTE_GRANT_A	MT7996_CAP_ROUTE(0x00)
/* Under the stock SYSRAM layout these hang the MCU; under the 256-bank ICAP
 * partition ("profile 5") they run but do not enlarge any instance's buffer
 * and change no landing. Firmware type 0 sets 0x30000 here.       [measured] */
#define   MT7996_CAP_ROUTE_GRANT_A_HI		HW_GENMASK(31, 16)
/* Hang on every tested layout. With the window route held: bits 4, 6, 7, 9,
 * 10, 11 hang; 0-3, 5, 8, 12-15 do not and change no landing.     [measured] */
#define   MT7996_CAP_ROUTE_GRANT_A_LO		HW_GENMASK(15, 0)
#define   MT7996_CAP_ROUTE_GRANT_A_LO_HANG	0x00000ed0u
#define   MT7996_CAP_ROUTE_GRANT_A_EVT_BREAK	HW_BIT(8)	/* with route words 0 */

/* ------------------------------------------------------------------------ */
/* +0x04 GRANT_B: window bank grants [31:24]; others DO NOT WRITE. Reset 0. */
/* ------------------------------------------------------------------------ */
#define MT7996_CAP_ROUTE_GRANT_B	MT7996_CAP_ROUTE(0x04)
/* Bit 24 + k grants window bank k (k = 0..7): 2 KiB = host page
 * 0x464000 + (k >> 1) * 0x1000, 32-bit lane (k & 1) of every 8 bytes
 * (512 words). Only granted banks are written. In beat terms (iq_format.h)
 * bank k holds beat word ((k >> 1) & 1) * 2 + (k & 1) of beats
 * (k >> 2) * 512 .. +511.                                          [measured] */
#define   MT7996_CAP_ROUTE_GRANT_B_WINDOW	HW_GENMASK(31, 24)
#define   MT7996_CAP_ROUTE_GRANT_B_BANK(k)	HW_BIT(24u + (unsigned int)(k))
#define   MT7996_CAP_ROUTE_BANK_PAGE(k)		(0x00464000u + ((unsigned int)(k) >> 1) * 0x1000u)
#define   MT7996_CAP_ROUTE_BANK_LANE(k)		((unsigned int)(k) & 1u)
#define   MT7996_CAP_ROUTE_GRANT_B_ALL		0xff000000u
/* Half hand-off during a ring: grant only one half. The released half keeps
 * its content, readable and unchanged; regranting resumes writes. [measured] */
#define   MT7996_CAP_ROUTE_GRANT_B_LOW_ONLY	0x0f000000u	/* beats 0..511 written */
#define   MT7996_CAP_ROUTE_GRANT_B_HIGH_ONLY	0xf0000000u	/* beats 512..1023 written */
/* Grants for 40 further banks, none in host-readable SRAM. Bits 2..9 each
 * hang the MCU alone; bits 0 and 1 alone do not, and are safe together with
 * GRANT_C bits 0, 1, 9, 31:19 only while the window grant [31:24] is held.
 * Combined grants change what the host reads at the SYSRAM alias
 * 0x400000..0x403fff. Firmware type 0 sets 0xffffff.               [measured] */
#define   MT7996_CAP_ROUTE_GRANT_B_OTHER	HW_GENMASK(23, 0)
#define   MT7996_CAP_ROUTE_GRANT_B_OTHER_SAFE	0x00000003u

/* ------------------------------------------------------------------------ */
/* +0x08 GRANT_C: non-window bank grants. Reset 0. DO NOT WRITE.            */
/* ------------------------------------------------------------------------ */
#define MT7996_CAP_ROUTE_GRANT_C	MT7996_CAP_ROUTE(0x08)
/* Stock layout: bits 2-8 and 10-18 hang (singly or by nibble); 0, 1, 9 and
 * 19-31 do not and change no landing. Under the 256-bank ICAP partition
 * [31:16] runs, [15:0] still hangs. Never touched by any firmware.
 *                                                         [measured] [static] */
#define   MT7996_CAP_ROUTE_GRANT_C_HANG		0x0007fdfcu
#define   MT7996_CAP_ROUTE_GRANT_C_HI		HW_GENMASK(31, 16)
#define   MT7996_CAP_ROUTE_GRANT_C_LO		HW_GENMASK(15, 0)

/* ------------------------------------------------------------------------ */
/* +0x0c OWNER: ten 3-bit owner fields, [29:0] writable. Reset 0.           */
/* ------------------------------------------------------------------------ */
#define MT7996_CAP_ROUTE_OWNER		MT7996_CAP_ROUTE(0x0c)
#define   MT7996_CAP_ROUTE_OWNER_FIELD(f) \
	HW_GENMASK(3u * (unsigned int)(f) + 2u, 3u * (unsigned int)(f))
/* Field 1: the instance that owns the window. Any nonzero value stops ICAP0
 * landing; with two instances triggered together only the owner lands. The
 * other fields change nothing for the window or the pool. Firmware type 0
 * sets fields 4-7 to 1 and field 8 to 2 (0x02249000).             [measured] */
#define   MT7996_CAP_ROUTE_OWNER_WINDOW		HW_GENMASK(5, 3)
#define     MT7996_CAP_ROUTE_OWNER_ICAP0	0u
#define     MT7996_CAP_ROUTE_OWNER_ICAP1	1u
#define     MT7996_CAP_ROUTE_OWNER_ICAP2	2u
#define   MT7996_CAP_ROUTE_OWNER_TYPE0		0x02249000u

/* +0x10: [2:0] writable storage, no effect.                        [measured] */
#define MT7996_CAP_ROUTE_10		MT7996_CAP_ROUTE(0x10)
/* +0x14, +0x18, +0x1c, +0x20: all bits writable storage; no bit changes the
 * landing. Firmware type 0 sets +0x18 |= 0x30001000, +0x1c |= 0x70005000,
 * +0x20 |= 0x1000 and never clears them.                  [measured] [static] */
#define MT7996_CAP_ROUTE_14		MT7996_CAP_ROUTE(0x14)
#define MT7996_CAP_ROUTE_18		MT7996_CAP_ROUTE(0x18)
#define   MT7996_CAP_ROUTE_18_TYPE0		0x30001000u
#define MT7996_CAP_ROUTE_1C		MT7996_CAP_ROUTE(0x1c)
#define   MT7996_CAP_ROUTE_1C_TYPE0		0x70005000u
#define MT7996_CAP_ROUTE_20		MT7996_CAP_ROUTE(0x20)
#define   MT7996_CAP_ROUTE_20_TYPE0		0x00001000u

/* +0x24 WINDOW: all bits writable. Firmware sets 0x02000000 on every capture
 * start and never clears it.                              [measured] [static] */
#define MT7996_CAP_ROUTE_WINDOW		MT7996_CAP_ROUTE(0x24)
/* Upper half of the window (beats 512..1023). Cleared: only beats 0..511
 * land. Other bits: storage.                                       [measured] */
#define   MT7996_CAP_ROUTE_WINDOW_UPPER		HW_BIT(25)

/* +0x28 WPATH: write-path enables, [21:0] writable. Reset 0.      [measured] */
#define MT7996_CAP_ROUTE_WPATH		MT7996_CAP_ROUTE(0x28)
/* ICAP0 into the UMAC pool (pool unit u = byte 4u, iq_format.h). Set: nothing
 * lands in the window.                                             [measured] */
#define   MT7996_CAP_ROUTE_WPATH_POOL		HW_BIT(0)
/* ICAP0 window write path. Cleared: nothing lands.                 [measured] */
#define   MT7996_CAP_ROUTE_WPATH_ICAP0		HW_BIT(10)
/* ICAP1 window write path.                                         [measured] */
#define   MT7996_CAP_ROUTE_WPATH_ICAP1		HW_BIT(21)
/* Firmware type 0 value; bits 14-17 are not needed for the pool. [measured] */
#define   MT7996_CAP_ROUTE_WPATH_TYPE0		0x0003c001u

/* +0x2c WPATH2: [21:0] writable. Reset 0.                         [measured] */
#define MT7996_CAP_ROUTE_WPATH2		MT7996_CAP_ROUTE(0x2c)
/* ICAP2 window write path (band 2; with +0x28 = 0).                [measured] */
#define   MT7996_CAP_ROUTE_WPATH2_ICAP2		HW_BIT(10)
#define   MT7996_CAP_ROUTE_WPATH2_TYPE0		0x00000180u

/*
 * Route configurations                                             [measured]
 *
 * Window, ICAP0 (the stock TESTMODE WM's route for every instance; hold it
 * across the trigger, release with +0x04 = 0):
 *   +0x04 = 0xff000000, +0x0c = 0, +0x24 = 0x02000000, +0x28 = 0x400
 * Window, ICAP1:  +0x04 = 0xff000000, +0x0c = 0x08, +0x24 = 0x02000000,
 *                 +0x28 = 0x200000
 * Window, ICAP2 (band 2): +0x04 = 0xff000000, +0x0c = 0x10,
 *                 +0x24 = 0x02000000, +0x28 = 0, +0x2c = 0x400
 * Pool, ICAP0: +0x28 = 0x1 suffices. The streaming setup writes the type-0
 *   words (without +0x00 and +0x04[23:0]): +0x04 = 0xff000000,
 *   +0x0c = 0x02249000, +0x18 = 0x30001000, +0x1c = 0x70005000,
 *   +0x20 = 0x1000, +0x24 = 0x02000000, +0x28 = 0x0003c001, +0x2c = 0x180.
 * Pool ICAP0 + window ICAP1 concurrently (all four band-0 chains):
 *   +0x28 = 0x200001, +0x0c[5:3] = 1, +0x04 = 0xff000000, +0x24[25] = 1.
 * Disarm after a stream: every word 0, then +0x24 = 0x02000000.
 *
 * Firmware behaviour                                                 [static]
 * The WM's route(inst, type, en) has three types. Type 1 (the only one the
 * SPECTRUM handler uses) writes +0x04 = 0xff000000 and per-instance
 * +0x0c / +0x24 / +0x28 / +0x2c; disable writes +0x04 = 0 only, so the rest
 * persists after a retire. The stock TESTMODE WM writes the ICAP0 values for
 * every instance; a per-instance variant ends on ICAP2's values (+0x0c =
 * 0x10, +0x28 = 0x200000, +0x2c = 0x400), which lands nothing for ICAP0.
 * Type 2 writes the same words as type 1. Type 0 ORs in the "TYPE0" values
 * above plus +0x00 |= 0x30000, +0x04 |= 0xffffff, and on disable does
 * +0x00 &= 0xffff, +0x04 &= 0xff000000; it never reads the instance.
 */

#endif /* HW_MT7996_CAP_ROUTE_H */
