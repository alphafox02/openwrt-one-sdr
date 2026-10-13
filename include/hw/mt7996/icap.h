/* SPDX-License-Identifier: MIT */
/*
 * MT7996 ICAP capture engines ICAP0/1/2: the hardware writer that moves DFE
 * capture-path samples into on-chip SRAM, 128 bits (one "beat") at a time.
 *
 * ADDRESS SPACE: WF bus, as seen by the WM/WA MCUs.
 *   ICAPn block      0x80023000 + n * 0x1000    (n = 0..2)
 *   ICAPCR0..12      block + 0x90 .. block + 0xc0
 * Host view: the fixed PCIe BAR0 window 0x80020000 -> BAR 0xb0000 (64 KiB,
 * WF_TOP_MISC) covers all three blocks, so ICAPn is BAR0 0xb3000 + n * 0x1000
 * with no remap (host.h).                                          [measured]
 * Nothing in any firmware or host source touches block offsets 0x00..0x8c or
 * 0xc4..0xffc.                                                       [static]
 *
 * The three instances have identical register masks and behaviour.
 * Per-instance differences are in what they can capture:
 *   ICAP0: band-0 streams 0-1 (or the band's two paths); the only instance
 *          that can write the UMAC pool (cap_route.h, +0x28[0]).   [measured]
 *   ICAP1: band-0 streams 2-3; window only.                         [measured]
 *   ICAP2: never strobes on band 0 (sees only the wide datapath); completes
 *          on band 2. Window only.                                  [measured]
 * Only one instance at a time can own the 16 KiB SYSRAM window (cap_route.h
 * +0x0c[5:3]); the other's writes are dropped.                     [measured]
 *
 * Where a beat lands is decided by the route block (cap_route.h); the beat
 * contents by the DFE capture mux and lane mux (dfe.h) and the CR10 packers
 * below; the memory layouts and sample format are in iq_format.h.
 *
 * Throughput: the writer accepts at most one 128-bit beat per 160 MHz clock
 * (640 M 32-bit samples/s, 20.48 Gbit/s).                          [measured]
 *
 * HAZARD: while CR0.EN is set, host reads of the SYSRAM window return each
 *   4 KiB page as its own last 8 bytes repeated 512 times. Clear EN (and the
 *   route grant, cap_route.h) before reading captured data.
 * HAZARD: the window route grant (cap_route.h +0x04) must be held across the
 *   trigger and until CAPTURE clears, or nothing lands; a route released
 *   during the capture silently drops the remaining beats.
 * HAZARD: in streaming use, stop ICAP only after the MCU_DMA0 TX ring that
 *   publishes its buffer has drained. Stopping ICAP first strands a TX
 *   descriptor (MCU_DMA0 error status bits 16-17) and wedges the WM until a
 *   firmware reload.
 * HAZARD: after a ring capture in which CR0.SWITCH was pulsed and EN then
 *   cleared, the next host read of the ICAP pool buffer (0xe0400000) caused a
 *   PCIe completion timeout that needed a power cycle; RESET_MT and reload
 *   failed. Cause not isolated: avoid SWITCH pulses in ring mode.
 * HAZARD: ADDR0 ranges outside [0, 0xffc] reach SRAM banks the MCU uses when
 *   the corresponding non-window route grants are set; with those grants an
 *   ICAP0 range at 0x4000 units hung the MCU (cap_route.h).
 *
 * Confidence tags: [measured] verified on hardware, [static] read from
 * ROM/firmware code or tables, [inferred].
 */
#ifndef HW_MT7996_ICAP_H
#define HW_MT7996_ICAP_H

#include "../hw_bits.h"

#define MT7996_ICAP_NUM			3u
#define MT7996_ICAP_BASE(n)		(0x80023000u + 0x1000u * (unsigned int)(n))
#define MT7996_ICAP_HOST_BAR0(n)	(0x000b3000u + 0x1000u * (unsigned int)(n))

/* ICAPCRk, k = 0..12, at block + 0x90 + 4k. */
#define MT7996_ICAP_CR(n, k)		(MT7996_ICAP_BASE(n) + 0x90u + 4u * (unsigned int)(k))
#define MT7996_ICAP_CR_NUM		13u

/* ------------------------------------------------------------------------ */
/* ICAPCR0 (+0x90): control and status.                                     */
/* Reset after firmware init 0x00000400. Writable: [3:0], 8, 10, 12, 13,    */
/* 19, [23:20]. Bits 7:4, 9, 11, 16, 29:24, 31 read 0 and drop writes.      */
/* ------------------------------------------------------------------------ */
#define MT7996_ICAP_CR0(n)		MT7996_ICAP_CR(n, 0)
#define   MT7996_ICAP_CR0_RESET			0x00000400u	/* [measured] */
/* Engine enable. Nothing is written while 0.                       [measured] */
#define   MT7996_ICAP_CR0_EN			HW_BIT(0)
/* Write 1 to start. Accepted only if EN was already 1 before this write
 * (0x400 -> 0x402 is dropped; 0x401 -> 0x403 starts). Reads 1 while pending;
 * self-clears when the write pointer passes ADDR0_END. Stays 1 forever in ring
 * mode or when the source produces no strobes.                     [measured] */
#define   MT7996_ICAP_CR0_CAPTURE		HW_BIT(1)
/* Storage: no effect on slot timing, inter-path phase or format at 40 Msps
 * or in hold modes; a sub-sample effect is unresolved.             [measured] */
#define   MT7996_ICAP_CR0_TIME_ADJ		HW_GENMASK(3, 2)
/* Storage: no effect on band 0 BW20 or on band 2.                  [measured] */
#define   MT7996_ICAP_CR0_WIFI_BW_MODE		HW_BIT(8)
/* Active-low soft reset. 0 holds the engine (no writes; EN/CAPTURE still
 * read back) and clears CR9 and WRAP. The firmware pulses it 0 -> 1 before
 * every capture, with no delay.                            [measured] [static] */
#define   MT7996_ICAP_CR0_SW_RST_B		HW_BIT(10)
/* Storage: no effect in any tested mode (single, ring, with ADDR1 set). The
 * ADDR1/ADDR2 chaining it names does not happen.                  [measured] */
#define   MT7996_ICAP_CR0_MODE_AB_SEL		HW_BIT(12)
/* Ring: at ADDR0_END the pointer wraps to ADDR0_START; CAPTURE never clears;
 * runs until EN = 0. CR9 is then the live pointer. Works at full rate, in the
 * window and in the pool.                                          [measured] */
#define   MT7996_ICAP_CR0_RING_BUF		HW_BIT(13)
/* Status, not storage (0x4400 reads 0x400): set during and after a ring
 * capture that wrapped; cleared by the next capture start.         [measured] */
#define   MT7996_ICAP_CR0_RING_WRAPPED		HW_BIT(14)
/* Status, not storage: seen after writes carrying bit 15 following a ring
 * capture; cleared by a write with EN = 1. Condition open.         [measured] */
#define   MT7996_ICAP_CR0_STS15			HW_BIT(15)
/* Sticky: set when the ring pointer wraps; cleared only by SW_RST_B = 0.
 * Read-only.                                                       [measured] */
#define   MT7996_ICAP_CR0_WRAP			HW_BIT(17)
/* Read-only, always 0 observed.                                    [measured] */
#define   MT7996_ICAP_CR0_STOP_SRC		HW_BIT(18)
/* Storage: no change to timing or format on band 0 BW20, band 2 or in hold
 * modes; never written by firmware.                       [measured] [static] */
#define   MT7996_ICAP_CR0_CLK_SEL		HW_BIT(19)
/* Sample source, from SPECTRUM request word 17 (spectrum_cmd.h).  [static] */
#define   MT7996_ICAP_CR0_SRC_SEL		HW_GENMASK(22, 20)
/* The DFE capture path (dfe.h). The only source that lands data. [measured] */
#define     MT7996_ICAP_CR0_SRC_SEL_DFE		0u
/* Named CAP_AFE by the host stack. Never strobes in any firmware mode, band
 * index (CR8 0..3), clock select or route, including the firmware's widest
 * route type 0; CAPTURE stays pending.                             [measured] */
#define     MT7996_ICAP_CR0_SRC_SEL_AFE		6u
/* Strobes like source 0, even with RX stopped (CR10[14:0] counts to the stop
 * cycle) but never completes a capture; content and clock unidentified.
 *                                                                  [measured] */
#define     MT7996_ICAP_CR0_SRC_SEL_7		7u
/* Values 1-5: no strobes, CAPTURE stays pending, nothing written. [measured] */
/* Storage: no effect set from the start or toggled mid-ring (rise, fall,
 * pulse), with or without MODE_AB_SEL. See the SWITCH-pulse HAZARD above.
 *                                                                  [measured] */
#define   MT7996_ICAP_CR0_SWITCH		HW_BIT(23)
/* Read-only, always 0 observed.                                    [measured] */
#define   MT7996_ICAP_CR0_MODE_R		HW_BIT(30)

/* Control words used for host-driven captures.                     [measured] */
#define   MT7996_ICAP_CR0_HELD			0x00000000u	/* reset held: clears CR9, WRAP */
#define   MT7996_ICAP_CR0_IDLE			0x00000400u	/* SW_RST_B, EN = 0; CR9 kept */
#define   MT7996_ICAP_CR0_ARM			0x00000401u	/* EN */
#define   MT7996_ICAP_CR0_TRIGGER		0x00000403u	/* EN | CAPTURE (after ARM) */
#define   MT7996_ICAP_CR0_RING_IDLE		0x00002400u
#define   MT7996_ICAP_CR0_RING_ARM		0x00002401u
#define   MT7996_ICAP_CR0_RING_TRIGGER		0x00002403u

/* ICAPCR1 (+0x94): not in the ROM register table. All 32 bits read 0,
 * writes dropped.                                         [measured] [static] */
#define MT7996_ICAP_CR1(n)		MT7996_ICAP_CR(n, 1)

/* ------------------------------------------------------------------------ */
/* Address registers ICAPCR2..7.                                            */
/*                                                                          */
/* Units of 4 bytes, four per 128-bit beat: beat = addr >> 2, bits [1:0]    */
/* ignored. Bits [17:2] address a 2^18-unit (1 MiB) space; bits [31:18] are */
/* ignored for the write but kept in CR9. With the window route, units      */
/* [0, 0xffc] are the 1024 beats of the SYSRAM window; with the pool route  */
/* (ICAP0 only) unit u is pool byte 4u (iq_format.h).               [measured] */
/* ------------------------------------------------------------------------ */
#define MT7996_ICAP_ADDR_UNIT_BYTES	4u
#define MT7996_ICAP_ADDR_UNITS_PER_BEAT	4u
#define MT7996_ICAP_ADDR_SPACE_UNITS	0x00040000u	/* 2^18 */
#define MT7996_ICAP_ADDR_SPACE_BYTES	0x00100000u	/* hard cap per instance */
#define MT7996_ICAP_ADDR_BEAT		HW_GENMASK(17, 2)
#define MT7996_ICAP_ADDR_ALIAS		HW_GENMASK(31, 18)
/* Address of byte offset `ofs` (a multiple of 16), and the inclusive END of
 * a buffer of `bytes` bytes starting at unit 0.                              */
#define MT7996_ICAP_ADDR_OF_BYTES(ofs)	((unsigned int)(ofs) >> 2)
#define MT7996_ICAP_ADDR_END_OF(bytes)	(((unsigned int)(bytes) >> 2) - 4u)

/* ICAPCR2 (+0x98) ADDR0_START. RW 32 bits. Reset 0x00100000.      [measured] */
#define MT7996_ICAP_CR2(n)		MT7996_ICAP_CR(n, 2)
#define   MT7996_ICAP_CR2_RESET			0x00100000u
/* ICAPCR3 (+0x9c) ADDR0_END, inclusive. RW 32 bits. Reset 0x0011fffc.
 * END < START: the pointer runs on through the 18-bit space, wrapping, and
 * never completes. END = START = 0 wrote one beat or none.         [measured] */
#define MT7996_ICAP_CR3(n)		MT7996_ICAP_CR(n, 3)
#define   MT7996_ICAP_CR3_RESET			0x0011fffcu
/* Window capture: ADDR0 = [0, 0xffc], 1024 beats.                 [measured] */
#define   MT7996_ICAP_WINDOW_START		0x00000000u
#define   MT7996_ICAP_WINDOW_END		0x00000ffcu

/* ICAPCR4 (+0xa0) ADDR1_START. RW 32 bits, reset 0x00120000. Never written
 * by the engine in any mode (single, ring, MODE_AB, SWITCH), with or without
 * real SRAM behind it: there is no hardware chaining.              [measured] */
#define MT7996_ICAP_CR4(n)		MT7996_ICAP_CR(n, 4)
#define   MT7996_ICAP_CR4_RESET			0x00120000u
/* ICAPCR5 (+0xa4) ADDR1_END by its ROM name; in practice the stop cycle.
 * RW 32 bits, reset 0x0013fffc. The ROM's stop-cycle setter writes here (its
 * table names CR10[14:0] instead); the firmware writes the request's stop
 * count >> 1, or >> 2 in four-samples-per-beat modes (ICAP2, u4BW 5).
 * CR10[14:0] counts strobes up to CR5[14:0]. No effect on how much lands:
 * captures end at ADDR0_END.                              [measured] [static] */
#define MT7996_ICAP_CR5(n)		MT7996_ICAP_CR(n, 5)
#define   MT7996_ICAP_CR5_RESET			0x0013fffcu
#define   MT7996_ICAP_CR5_STOP_CYCLE		HW_GENMASK(14, 0)
/* ICAPCR6/7 (+0xa8/+0xac) ADDR2_START/END. RW 32 bits, reset 0x00140000 /
 * 0x0015fffc. Never used by the engine; never written by firmware.
 *                                                         [measured] [static] */
#define MT7996_ICAP_CR6(n)		MT7996_ICAP_CR(n, 6)
#define   MT7996_ICAP_CR6_RESET			0x00140000u
#define MT7996_ICAP_CR7(n)		MT7996_ICAP_CR(n, 7)
#define   MT7996_ICAP_CR7_RESET			0x0015fffcu

/* ------------------------------------------------------------------------ */
/* ICAPCR8 (+0xb0): source band and debug. Reset 0. Bit 30 reads 0.         */
/* ------------------------------------------------------------------------ */
#define MT7996_ICAP_CR8(n)		MT7996_ICAP_CR(n, 8)
/* Which band's DFE capture path the engine taps: the SPECTRUM band/phy
 * index. ROM names [0] WF_BANK_SEL and [1] AFE_SEL; neither is a memory bank
 * nor an ADC select. 0 = band 0; 1..3 on a band-0 setup give no strobes; the
 * firmware writes 2 for a band-2 capture. The firmware writes the whole word
 * (0, or 2 for index >= 2) and then sets bit 0 from the index.
 *                                                         [measured] [static] */
#define   MT7996_ICAP_CR8_BAND			HW_GENMASK(1, 0)
#define   MT7996_ICAP_CR8_WF_BANK_SEL		HW_BIT(0)
#define   MT7996_ICAP_CR8_AFE_SEL		HW_BIT(1)
/* Storage; cleared by every firmware write.                        [measured] */
#define   MT7996_ICAP_CR8_RESERVED		HW_GENMASK(18, 2)
/* Storage: every single bit tried, no effect.                      [measured] */
#define   MT7996_ICAP_CR8_INTERNAL_DBG_SEL	HW_GENMASK(26, 19)
/* Storage, FPGA-only by name.                                      [measured] */
#define   MT7996_ICAP_CR8_FPGA_LOWRATE		HW_BIT(27)
#define   MT7996_ICAP_CR8_FPGA_USE		HW_GENMASK(29, 28)
/* Storage.                                                         [measured] */
#define   MT7996_ICAP_CR8_NON_1ST_TIME_FLAG	HW_BIT(31)

/* ICAPCR9 (+0xb4) WIFI_DGB_STOP_ADDR (ROM spelling). Read-only, writes
 * dropped; cleared by SW_RST_B = 0. Address (units) of the last beat written,
 * full 32 bits (START's upper bits kept). Single capture: ADDR0_END at
 * completion. Ring: the live write pointer, advancing 4 units per beat (80 M
 * units/s at 2 x 40 Msps) and wrapping. No half-done status exists, so a ring
 * consumer polls this.                                             [measured] */
#define MT7996_ICAP_CR9(n)		MT7996_ICAP_CR(n, 9)
/* Byte offset (within a power-of-two ring of `ring` bytes starting at unit 0)
 * one past the last beat written.                                            */
#define   MT7996_ICAP_CR9_NEXT_BYTE(cr9, ring) \
	((((unsigned int)(cr9) << 2) + 16u) & ((unsigned int)(ring) - 1u))

/* ------------------------------------------------------------------------ */
/* ICAPCR10 (+0xb8): input packers and status. Reset 0.                     */
/* ------------------------------------------------------------------------ */
#define MT7996_ICAP_CR10(n)		MT7996_ICAP_CR(n, 10)
/* Strobe counter: counts source strobes up to CR5[14:0] and reads CR5[14:0]
 * after a capture; 0 if the source never strobes. Not writable.   [measured] */
#define   MT7996_ICAP_CR10_STOP_CYCLE		HW_GENMASK(14, 0)
/* Packers. Each takes the first N 32-bit words of the lane-mux output per
 * strobe (slot 0 = path 0 sample t, 1 = path 0 t+1, 2 = path 1 t, 3 = path 1
 * t+1 on the 2-sample datapath) and packs them densely into beats. None set
 * = all four slots per beat. Layouts: iq_format.h. The firmware picks one per
 * instance (ICAP0/1 none, ICAP2 64->128) and only ever sets, never clears,
 * so a stale enable survives a later capture.  [measured] [static] [inferred] */
#define   MT7996_ICAP_CR10_64B_128B_EN		HW_BIT(16)	/* first 2 words */
#define   MT7996_ICAP_CR10_32B_64B_EN		HW_BIT(17)	/* first 1 word, high half 0 */
#define   MT7996_ICAP_CR10_24B_32B_EN		HW_BIT(18)	/* low 24 bits of word 0 */
#define   MT7996_ICAP_CR10_96B_128B_EN		HW_BIT(19)	/* first 3 words */
#define   MT7996_ICAP_CR10_PACK			HW_GENMASK(19, 16)
/* Read-only status, named by the ROM; 0 in every read (out-of-window, START >
 * END, ring, every route trial). Never read by firmware.
 *                                                         [measured] [static] */
#define   MT7996_ICAP_CR10_RO_SRAM_PTR_FIFO_EMPTY HW_BIT(23)
#define   MT7996_ICAP_CR10_RO_SRAM_PTR_FIFO_FULL  HW_BIT(24)
#define   MT7996_ICAP_CR10_RO_AXI_ERROR		HW_BIT(25)
/* Host-stack name SYSRAM_INTF_SEL2; storage, no effect. Its neighbours 26 and
 * 28 (SEL1/SEL3) are not implemented (read 0).                     [measured] */
#define   MT7996_ICAP_CR10_SYSRAM_INTF_SEL2	HW_BIT(27)

/* ICAPCR11 (+0xbc) SYSRAM_OFFSET. RW 32 bits, reset 0. No bit or value moves
 * or blocks the landing; never written by firmware.       [measured] [static] */
#define MT7996_ICAP_CR11(n)		MT7996_ICAP_CR(n, 11)
/* ICAPCR12 (+0xc0) PSE_END_ADDR. RW 32 bits, reset 0xa8027fff. No value tried
 * (0, all-ones, 0x27fff, 0xfff, 0x1000) changes the landing.      [measured] */
#define MT7996_ICAP_CR12(n)		MT7996_ICAP_CR(n, 12)
#define   MT7996_ICAP_CR12_RESET		0xa8027fffu

/*
 * Sequences                                                        [measured]
 *
 * One window capture (ICAP0, route held as in cap_route.h, DFE path already
 * programmed by a firmware SPECTRUM prime, spectrum_cmd.h):
 *   CR0 = IDLE; CR2 = 0; CR3 = 0xffc; CR10 = packer (0 = both paths);
 *   route hold; CR0 = ARM; CR0 = TRIGGER; poll CR0.CAPTURE == 0
 *   (51.2 us at 2 x 40 Msps; 6.4 us at 640 Msps); route release
 *   (+0x04 = 0); CR0 = IDLE; then read the window.
 * Re-triggering without a firmware command or reset rewrites the window.
 *
 * Ring into the pool (ICAP0, pool route):
 *   CR2 = 0; CR3 = ADDR_END_OF(ring bytes); CR10 = packer;
 *   CR0 = HELD; CR0 = RING_IDLE; CR0 = RING_ARM; CR0 = RING_TRIGGER;
 *   follow CR9 (CR9_NEXT_BYTE); stop with CR0 = IDLE (EN = 0, CR9 stays
 *   readable) only after downstream DMA has drained.
 * Releasing one half of the window's bank grants during a ring freezes that
 * half readable while the writer continues in the other half; its writes to
 * the released half are dropped, and the pointer does not wait.
 */

#endif /* HW_MT7996_ICAP_H */
