/* SPDX-License-Identifier: MIT */
/*
 * MT7996 DFE capture path, per band: the capture mux that selects a tap of
 * the receive chain, its sample-rate/hold mode and enables, and the lane mux
 * that orders lanes into ICAP beat slots (icap.h). Also the capture node keys
 * that program it, and the separate DFE calibration capture engine that
 * writes a DSP's DTCM.
 *
 * ADDRESS SPACE: WF bus, as seen by the WM. Band b (= the capture's phy_idx)
 * at +b * 0x100000:
 *   capture mux   0x830ad440 + b * 0x100000 .. 0x830ad46c
 * Not in a fixed host BAR window; the host reaches it through firmware
 * REG_ACCESS (UNI 0x0d, mcu_cmd.h). Register masks are identical for the
 * three banks.                                                     [measured]
 *
 * Measured conditions: band 0 on a 20 MHz channel with node 0x00204000 and
 * ICAP source 0 unless stated; band 2 on a 6 GHz 320 MHz channel for the
 * lane mux. "Stall" = the ICAP sees no strobes and CAPTURE stays pending.
 *
 * Field names (SEL, PHY1SEL, BwSampleRate, Mode, PHY1Mode) are from the
 * firmware's node-hit log string. No physical tap or chain names follow from
 * the node table; the tap characterisation below is measured.
 *
 * Confidence tags: [measured] verified on hardware, [static] read from
 * firmware code or tables, [inferred].
 */
#ifndef HW_MT7996_DFE_H
#define HW_MT7996_DFE_H

#include "../hw_bits.h"

#define MT7996_DFE_BAND_NUM		3u
#define MT7996_DFE_BAND_OFS(b)		((unsigned int)(b) << 20)
#define MT7996_DFE_CAP(b, ofs)		(0x830ad000u + MT7996_DFE_BAND_OFS(b) + (unsigned int)(ofs))

/* ------------------------------------------------------------------------ */
/* +0x440 MODE: capture mode code. Writable [7:0], [29:20].                 */
/* ------------------------------------------------------------------------ */
#define MT7996_DFE_CAP_MODE(b)		MT7996_DFE_CAP(b, 0x440)
/* Node record activation code (byte +0x0b, +0x0c when PHY index = 1). With the domain-0 setup only
 * 0x11 works; all 255 other values stall. Domain 1 runs with 0x14 plus
 * MUX[24]. Firmware assignments: 0x11 domain 0, 0x14 domain 1, 0x21
 * domain 2, 0x31 domains 6/7.                             [measured] [static] */
#define   MT7996_DFE_CAP_MODE_CODE		HW_GENMASK(7, 0)
#define     MT7996_DFE_MODE_DOMAIN0		0x11u
#define     MT7996_DFE_MODE_DOMAIN0_ALT		0x13u	/* coded, no node uses it [static] */
#define     MT7996_DFE_MODE_DOMAIN1		0x14u
#define     MT7996_DFE_MODE_DOMAIN2		0x21u	/* 0x20 also decoded [static] */
#define     MT7996_DFE_MODE_DOMAIN67		0x31u
/* Output gate: set, every sample is written as 0.                  [measured] */
#define   MT7996_DFE_CAP_MODE_ZERO		HW_BIT(28)
/* Set: stall.                                                      [measured] */
#define   MT7996_DFE_CAP_MODE_STALL		HW_BIT(29)
/* Bits 20-27: storage, no effect.                                  [measured] */
#define   MT7996_DFE_CAP_MODE_SPARE		HW_GENMASK(27, 20)

/* +0x444: [1:0] writable; either bit set stalls the strobe. Never written by
 * firmware.                                               [measured] [static] */
#define MT7996_DFE_CAP_444(b)		MT7996_DFE_CAP(b, 0x444)
#define   MT7996_DFE_CAP_444_STALL		HW_GENMASK(1, 0)

/* ------------------------------------------------------------------------ */
/* +0x448 MUX: SEL / rate / activation for domains 0 and 1.                 */
/* Observed: 0x00230014 band 0 after a node 0x00204000 u4BW-0 prime;        */
/* 0x00200014 band 2 after u4BW 5.                                [measured] */
/* ------------------------------------------------------------------------ */
#define MT7996_DFE_CAP_MUX(b)		MT7996_DFE_CAP(b, 0x448)
/* Domain-0 SEL: the tap. Live only at 0x05, 0x14, 0x18 (see Taps); the other
 * 253 values write the constant word 0x00442233.                  [measured] */
#define   MT7996_DFE_CAP_MUX_SEL0		HW_GENMASK(7, 0)
#define     MT7996_DFE_SEL0_PRE_FILTER		0x05u
#define     MT7996_DFE_SEL0_STOCK		0x14u
#define     MT7996_DFE_SEL0_FILTERED		0x18u
#define     MT7996_DFE_SEL0_DEAD_WORD		0x00442233u
/* Domain-1 SEL (with MODE 0x14 and MUX[24]): live at 0x00, 0x04, 0x08, 0x0c;
 * 0x1c and 0x40 write zeros; the other 250 write the constant 0x3eaa2aaa.
 * No effect while domain 0 is the active one.                     [measured] */
#define   MT7996_DFE_CAP_MUX_SEL1		HW_GENMASK(15, 8)
#define     MT7996_DFE_SEL1_DEAD_WORD		0x3eaa2aaau
/* Domain-0 BwSampleRate m: the engine samples each path at 40 * 2^(3-m) MHz.
 * The data keep the channel's rate (2 x channel bandwidth): sampling faster
 * holds (repeats) each value, sampling slower decimates with no filter. On a
 * 20 MHz channel m = 3, 2, 1, 0 repeat each value 1, 2, 4, 8 times. The
 * SPECTRUM u4BW word selects m through the node record (spectrum_cmd.h).
 *                                                                  [measured] */
#define   MT7996_DFE_CAP_MUX_RATE0		HW_GENMASK(17, 16)
#define   MT7996_DFE_MODE_SAMPLE_HZ(m)		(40000000u << (3u - (unsigned int)(m)))
/* Domain-1 BwSampleRate, same law (m = 0..3 hold 8/4/2/1 on BW20). [measured] */
#define   MT7996_DFE_CAP_MUX_RATE1		HW_GENMASK(19, 18)
/* Domain-0 activation (MODE 0x11). Cleared: stall.                 [measured] */
#define   MT7996_DFE_CAP_MUX_ACT0		HW_BIT(21)
/* Activation for MODE 0x13; no node uses it. Storage on hardware.
 *                                                         [static] [measured] */
#define   MT7996_DFE_CAP_MUX_ACT0_ALT		HW_BIT(23)
/* Domain-1 activation (MODE 0x14).                                 [measured] */
#define   MT7996_DFE_CAP_MUX_ACT1		HW_BIT(24)
/* Bits 20, 22, 25, 28: storage, no effect.                         [measured] */

/* ------------------------------------------------------------------------ */
/* +0x44c MUX2: domains 2 and 6/7. Writable mask 0xf1f700ff.                */
/* Every domain-2 combination tried (SEL 0, 4, 8, 9, 10 x rate 0-7; all 256 */
/* SEL at rate 4) and every domain-6/7 SEL x rate stalls on band 0; no      */
/* effect on domains 0/1.                                          [measured] */
/* ------------------------------------------------------------------------ */
#define MT7996_DFE_CAP_MUX2(b)		MT7996_DFE_CAP(b, 0x44c)
#define   MT7996_DFE_CAP_MUX2_WRITABLE		0xf1f700ffu
#define   MT7996_DFE_CAP_MUX2_SEL2		HW_GENMASK(7, 0)
#define   MT7996_DFE_CAP_MUX2_RATE2		HW_GENMASK(18, 16)
#define   MT7996_DFE_CAP_MUX2_ACT2		HW_BIT(21)	/* MODE 0x20/0x21 */
#define   MT7996_DFE_CAP_MUX2_ACT67		HW_BIT(24)	/* MODE 0x31 */
#define   MT7996_DFE_CAP_MUX2_SEL67		HW_GENMASK(29, 28)
#define   MT7996_DFE_CAP_MUX2_RATE67		HW_GENMASK(31, 30)

/* +0x450..+0x45c: second copy of +0x440..+0x44c (PHY1SEL / PHY1Mode by the
 * log names), same writable masks; storage, no effect on a phy-0 capture.
 * Never written by firmware.                              [measured] [static] */
#define MT7996_DFE_CAP_PHY1_MODE(b)	MT7996_DFE_CAP(b, 0x450)
#define MT7996_DFE_CAP_PHY1_454(b)	MT7996_DFE_CAP(b, 0x454)
#define MT7996_DFE_CAP_PHY1_MUX(b)	MT7996_DFE_CAP(b, 0x458)
#define MT7996_DFE_CAP_PHY1_MUX2(b)	MT7996_DFE_CAP(b, 0x45c)

/* ------------------------------------------------------------------------ */
/* +0x460 CTRL: writable mask 0x37. Reads 0x2 after a firmware prime.       */
/* ------------------------------------------------------------------------ */
#define MT7996_DFE_CAP_CTRL(b)		MT7996_DFE_CAP(b, 0x460)
#define   MT7996_DFE_CAP_CTRL_WRITABLE		0x00000037u
/* Freeze: one constant sample is written.                          [measured] */
#define   MT7996_DFE_CAP_CTRL_FREEZE		HW_BIT(0)
/* Enable; cleared = stall. Set by the firmware before every capture and
 * never cleared by it.                                    [measured] [static] */
#define   MT7996_DFE_CAP_CTRL_EN		HW_BIT(1)
/* Each pair holds one sample twice.                                [measured] */
#define   MT7996_DFE_CAP_CTRL_PAIR_HOLD		HW_BIT(2)
/* Bits 4, 5: storage, no effect.                                   [measured] */

/* +0x464 PATH: [7:0] writable. Never written by firmware.[measured] [static] */
#define MT7996_DFE_CAP_PATH(b)		MT7996_DFE_CAP(b, 0x464)
#define   MT7996_DFE_CAP_PATH_STALL		HW_BIT(0)
/* Path 1 (beat high half) written as 0.                            [measured] */
#define   MT7996_DFE_CAP_PATH_P1_ZERO		HW_BIT(1)
/* Bits 2-7: no effect.                                             [measured] */

/* ------------------------------------------------------------------------ */
/* +0x468 LANE: lane mux. Writable mask 0x1f1f1f1f, reset 0x18100800.       */
/* Acts only on the wide (4 samples per 160 MHz clock per path) datapath of */
/* band 2 on a 320 MHz channel; no effect at all on band 0 BW20, even after */
/* toggling every enable.                                          [measured] */
/*                                                                          */
/* On the wide datapath a lane is one 64-bit unit holding two samples of    */
/* one path. Byte 0 selects the lane written as the beat's low half (words  */
/* 0-1), byte 1 the high half (words 2-3); bytes 2 and 3 have no effect.    */
/* Byte 0 = byte 1 gives two identical halves.                     [measured] */
/* ------------------------------------------------------------------------ */
#define MT7996_DFE_CAP_LANE(b)		MT7996_DFE_CAP(b, 0x468)
#define   MT7996_DFE_CAP_LANE_WRITABLE		0x1f1f1f1fu
#define   MT7996_DFE_CAP_LANE_RESET		0x18100800u
#define   MT7996_DFE_CAP_LANE_LO		HW_GENMASK(4, 0)
#define   MT7996_DFE_CAP_LANE_HI		HW_GENMASK(12, 8)
#define   MT7996_DFE_CAP_LANE_B2		HW_GENMASK(20, 16)
#define   MT7996_DFE_CAP_LANE_B3		HW_GENMASK(28, 24)
/* Lane value (5 bits): [2:0] path, bit 3 pair, bit 4 ignored (16 == 0).
 * Pair 0 = samples t, t+2; pair 1 = t+1, t+3. Path 0..3 carry signal with RX
 * mask 0xf (paths 2, 3 about 2x path 0's amplitude, each with its own
 * constant offset); paths 4..7 are written as 0 with mask 0xf (path 4 is the
 * fifth 6 GHz receiver).                                           [measured] */
#define   MT7996_DFE_LANE_PATH			HW_GENMASK(2, 0)
#define   MT7996_DFE_LANE_ODD			HW_BIT(3)
#define   MT7996_DFE_LANE_IGNORED		HW_BIT(4)
#define   MT7996_DFE_LANE(path, odd) \
	((((unsigned int)(odd) & 1u) << 3) | ((unsigned int)(path) & 7u))
#define   MT7996_DFE_CAP_LANE_WORD(lo, hi)	((unsigned int)(lo) | ((unsigned int)(hi) << 8))
/* Written by the firmware for u4BW 5 with node 0x00204000: path 0 even pair
 * low, odd pair high = one 640 Msps stream (iq_format.h "quad").
 *                                                         [measured] [static] */
#define   MT7996_DFE_CAP_LANE_640M		0x09010800u
/* The firmware's lane table pairs (p, p + 8) for p = 0..4
 * (0x18100800 .. 0x1c140c04). Every SPECTRUM arm writes the register
 * (capture_setup.h "Lane word"): MT7996_DFE_CAP_LANE_FW_DEFAULT 0x03020100
 * unless the node / u4BW lane rule applies (the stock node: LANE_640M at
 * u4BW 5).                                                            [static] */
#define   MT7996_DFE_CAP_LANE_FW_PATH(p)	(0x18100800u + 0x01010101u * (unsigned int)(p))
#define   MT7996_DFE_CAP_LANE_FW_DEFAULT	0x03020100u

/* +0x46c LANE2: writable mask 0x1f1f1f1f, reset 0x07060504. No byte changes
 * the window on band 0 or band 2. Never written by firmware.
 *                                                         [measured] [static] */
#define MT7996_DFE_CAP_LANE2(b)		MT7996_DFE_CAP(b, 0x46c)
#define   MT7996_DFE_CAP_LANE2_RESET		0x07060504u

/* +0xc00 (0x830adc00 + b * 0x100000): [19:18] set to 3 by the stock firmware
 * only for domain-6 nodes and cleared for all others; shared with DFE
 * calibration routines. Inferred: selects the tap feeding domain 6. Setting
 * it with a domain-6 node did not produce a capture.   [static] [inferred]  */
#define MT7996_DFE_CAP_D6(b)		MT7996_DFE_CAP(b, 0xc00)
#define   MT7996_DFE_CAP_D6_SEL			HW_GENMASK(19, 18)

/* ------------------------------------------------------------------------ */
/* Capture nodes                                                            */
/*                                                                          */
/* The SPECTRUM node word (spectrum_cmd.h) is looked up by exact key in a   */
/* 53-record firmware table. A record programs, on bank phy_idx: the        */
/* domain's SEL from byte +4 (+5 when PHY index = 1), its rate field from   */
/* the per-u4BW byte (+6/+7/+8 for u4BW 0/1/2, +9 for 3 and 4, +0xa for 5), */
/* and MODE [7:0] = the activation code from byte +0x0b (+0x0c when PHY    */
/* index = 1). For a key not in the table MODE [7:0] = the node's config    */
/* field [19:12], and CTRL, LANE and CAPTRIG are still written; only the    */
/* mux fields are skipped (capture_setup.h).                        [static] */
/* ------------------------------------------------------------------------ */
#define MT7996_DFE_NODE_DOMAIN		HW_GENMASK(31, 28)
#define MT7996_DFE_NODE_CATEGORY	HW_GENMASK(27, 20)
#define MT7996_DFE_NODE_CONFIG		HW_GENMASK(19, 12)
#define MT7996_DFE_NODE(dom, cat, cfg) \
	(((unsigned int)(dom) << 28) | ((unsigned int)(cat) << 20) | ((unsigned int)(cfg) << 12))

/* Domain -> fields. 0: MUX SEL0/RATE0/ACT0; 1: MUX SEL1/RATE1/ACT1;
 * 2: MUX2 SEL2/RATE2/ACT2; 6 and 7: MUX2 SEL67/RATE67/ACT67.         [static] */

/* Domain 0. Rate bytes per u4BW 0..5 in brackets.          [static] [measured] */
#define MT7996_DFE_NODE_D0_SEL05	0x00004000u	/* SEL 0x05 [0,0,0,0,0,0] */
#define MT7996_DFE_NODE_STOCK		0x00204000u	/* SEL 0x14 [3,2,1,0,0,0] */
#define MT7996_DFE_NODE_D0_SEL18	0x00304000u	/* SEL 0x18 [3,2,1,0,0,0] */
/* Domain 1.                                                          [static] */
#define MT7996_DFE_NODE_D1_SEL00	0x10404000u	/* SEL 0x00 [3,2,1,0,0,0] */
#define MT7996_DFE_NODE_D1_SEL04	0x10504000u	/* SEL 0x04 [3,2,1,0,0,0] */
#define MT7996_DFE_NODE_D1_SEL08	0x10604000u	/* SEL 0x08 [3,2,1,0,0,0] */
#define MT7996_DFE_NODE_D1_SEL0C	0x10704000u	/* SEL 0x0c [3,3,2,1,1,0] */
#define MT7996_DFE_NODE_D1_SEL1C	0x10804000u	/* SEL 0x1c [3,3,2,1,1,0]; zeros */
/* Domains 6/7 (activation 0x31). Inferred: taps of a second receive path; on
 * band 0 that is chain 4, the monitor (DFS/scan) receiver. 0x60024000
 * streams only zeros into ICAP0, also with the monitor receiver and RDD 2
 * running and CAP_D6_SEL = 3.                  [static] [measured] [inferred] */
/* HAZARD: a firmware channel switch with RX mask 0x10 (chain 4 alone)
 *   crashes the firmware; one MCU hang followed the domain-6 / RDD set-up
 *   (recovered by a firmware reload).                              [measured] */
#define MT7996_DFE_NODE_D6_MONITOR	0x60024000u	/* SEL 0 [0,0,0,0,0,0] */
#define MT7996_DFE_NODE_D6_SEL1		0x60224000u	/* SEL 1 [3,2,1,0,0,0] */
#define MT7996_DFE_NODE_D7_SEL2		0x70524000u	/* SEL 2 [3,2,1,0,0,0] */
#define MT7996_DFE_NODE_D7_SEL3		0x70624000u	/* SEL 3 [3,2,1,0,0,0] */
#define MT7996_DFE_NODE_D6_CAT13	0x60d24000u	/* SEL 0 [0,0,0,0,0,0] */
/* Domain-2 nodes are 0x2xxxx000 (categories 9, 10, 14, 15, 16); all stall on
 * band 0.                                                 [static] [measured] */

/*
 * Taps: what the live SEL values select                            [measured]
 *
 * SEL values are points along the receive chain, not antenna chains. With a
 * single-chain RX mask (1, 2, 4 or 8) every tap carries the one enabled chain
 * in the beat's low half and writes the high half as 0: path 0/1 = active
 * stream 0/1, whichever chains are on. Every tap runs at the channel rate.
 * Noise spectrum on a 20 MHz channel, relative to 1-8 MHz:
 *
 *   tap               8-9.5 MHz  10.5-12  12-20   rms     reading
 *   D0 SEL 0x05        -0.4 dB    -3.1    -5.6   ~2660   before the channel filter
 *   D0 SEL 0x14 stock  -2.0       -5.1   -12.9   ~380    partly filtered
 *   D0 SEL 0x18        -2.8      -13.7   -23.6   ~300    channel-filtered
 *   D1 SEL 0/4/8       ~-10       ~-15    ~-24   ~950    channel-filtered
 *   D1 SEL 0x0c        +5.6       -6.1   -35.9   ~910    sharp band edge, lift near it
 *
 * The path amplitude ratio and inter-path phase switch between two states
 * from capture to capture (receiver AGC); they are not register effects.
 *
 * Response of the stock tap (SEL 0x14) at the 160 MHz class, 320 Msps: band
 * 1, MT7977 analog filter at 320 MHz (so the digital path dominates), gain
 * forced LNA 2 / LPF 5, a B205 tone fixed at 5250 MHz and the LO stepped; dB
 * relative to 5 MHz off the LO (2026-10-11):
 *   off (MHz)   40   60   80  100  120  130   140   150   155
 *              0.5  0.7  0.6 -2.2 -6.8 -10.1 -14.1 -19.3 -22.9
 * Tones beyond +-160 MHz alias in about 40 dB down (MT7977 filter at 160
 * MHz: 49 dB). Neither the other front-end coefficient set (phy.h DFE
 * +0xf020..+0xf030, the BW20 / BW320 values) nor any manual DLPF mode
 * (phy.h MT7996_PHY_RXFE01_DLPF_MODE_MAN_EN with CTL 0..7) moves it by more
 * than about 1 dB. SEL 0x05 at this class: the samples clip at +-4094 and the
 * response is irregular (-21.5 dB at 20 MHz, 0.3 at 130, aliases at -23 dB):
 * not a usable wide tap as configured; not characterised further. [measured]
 */

/* ------------------------------------------------------------------------ */
/* DFE calibration capture engine ("MemDPD"): writes IQ into band b's DSP   */
/* DTCM (dsp.h). Not part of the ICAP path. The stock arm programs it,      */
/* pulses it and triggers it; on RX alone it arms and runs (X_STS[12]) but  */
/* never sets done and writes no DTCM word in any mode 0..3, with or        */
/* without EN2. Inferred: it needs the TX calibration/loopback source; the  */
/* stock OTFIQ calibration (phy_cal.h T2{8}) arms it while transmitting.    */
/* The stock DPD calibration (phy_cal.h T2{0..3}, steps 0..7) does not      */
/* touch it: DPD trains with its own engine at WF_COMM +0xe000              */
/* (phy_cal.h MT7996_PHY_DPD_CTRL); the MemDPD steps are not decoded.       */
/*                                                [static] [measured] [inferred] */
/* ------------------------------------------------------------------------ */
/* Bit 0 enable; [9:8] mode/source (vendor value 2). Rest 0x80; the stock arm
 * writes 0x80000081 | mode << 8, then the same without bit 31.     [measured] */
#define MT7996_DFE_XCAP_CTRL(b)		MT7996_DFE_CAP(b, 0x594)
#define   MT7996_DFE_XCAP_CTRL_EN		HW_BIT(0)
#define   MT7996_DFE_XCAP_CTRL_MODE		HW_GENMASK(9, 8)
#define   MT7996_DFE_XCAP_CTRL_ARM		HW_BIT(31)
/* Second enable, bit 0; the OTFIQ arm also sets bit 3 (reads 9 after it).
 *                                                         [static] [measured] */
#define MT7996_DFE_XCAP_EN2(b)		MT7996_DFE_CAP(b, 0x5cc)
#define   MT7996_DFE_XCAP_EN2_EN		HW_BIT(0)
#define   MT7996_DFE_XCAP_EN2_B3		HW_BIT(3)
/* Destination start / end, in the PHY-side view 0x80200000 + DTCM offset:
 * X buffer 0x80206000..0x8020afff, Y 0x8020b000..0x8020ffff, end
 * 0x80210000. Rest 0.                                     [static] [measured] */
#define MT7996_DFE_XCAP_START(b)	MT7996_DFE_CAP(b, 0x5d0)
#define MT7996_DFE_XCAP_END(b)		MT7996_DFE_CAP(b, 0x5d4)
#define   MT7996_DFE_XCAP_DST_X			0x80206000u
#define   MT7996_DFE_XCAP_DST_Y			0x8020b000u
#define   MT7996_DFE_XCAP_DST_END		0x80210000u
/* [4:0] SRAM buffer check: nonzero aborts the firmware's computation.
 * Rest 0.                                                 [static] [measured] */
#define MT7996_DFE_XCAP_STATUS(b)	MT7996_DFE_CAP(b, 0xc2c)
#define   MT7996_DFE_XCAP_STATUS_ERR		HW_GENMASK(4, 0)
/* X / Y capture status. Bit 30 done (firmware prints a failure if clear);
 * bit 12 set while triggered, cleared on release. Rest 0x4000.
 *                                                         [static] [measured] */
#define MT7996_DFE_XCAP_X_STS(b)	MT7996_DFE_CAP(b, 0xeb0)
#define MT7996_DFE_XCAP_Y_STS(b)	MT7996_DFE_CAP(b, 0xeb4)
#define   MT7996_DFE_XCAP_STS_RUN		HW_BIT(12)
#define   MT7996_DFE_XCAP_STS_DONE		HW_BIT(30)
/* Trigger: bit 31 held for the capture; bit 26 pulsed (set, 10 us, clear)
 * during the arm. Rest 0x40020000.                        [static] [measured] */
#define MT7996_DFE_XCAP_TRIG(b)		MT7996_DFE_CAP(b, 0xe00)
#define   MT7996_DFE_XCAP_TRIG_GO		HW_BIT(31)
#define   MT7996_DFE_XCAP_TRIG_PULSE		HW_BIT(26)
/* Bit 13 set during the arm. Rest 0x07000100. The OTFIQ arm pulses bit 13
 * (no delay), then writes [3:0] = 1 and [31:16] = 0 (reads 0x101 after it).
 *                                                         [static] [measured] */
#define MT7996_DFE_XCAP_BD0(b)		MT7996_DFE_CAP(b, 0xbd0)
#define   MT7996_DFE_XCAP_BD0_ARM		HW_BIT(13)
#define   MT7996_DFE_XCAP_BD0_3_0		HW_GENMASK(3, 0)
#define   MT7996_DFE_XCAP_BD0_31_16		HW_GENMASK(31, 16)
/* 0x8300e81c + b * 0x100000: bit 31 pulsed (set, 10 us, clear) during the
 * arm. Rest 0.                                            [static] [measured] */
#define MT7996_DFE_XCAP_PULSE(b)	(0x8300e81cu + MT7996_DFE_BAND_OFS(b))
#define   MT7996_DFE_XCAP_PULSE_BIT		HW_BIT(31)
/* Completion goes to band b's DSP as its MEMDPD interrupt; the stock arm
 * polls the DSP's MEMDPD tag (dsp.h) for bit 31.                     [static] */
/* OTFIQ arm (V:e0046040, phy_cal.h T2{8}; band 2 BW320 in the stock load):
 * START = 0x80206000, END = 0x80210000 (the DSP's X and Y buffers), CTRL
 * [0] = 1 and MODE = 2, EN2 [0] and [3], BD0 as above, CTRL = 0x80000281
 * then 0x281; the band's test-signal generator then transmits for each DSP
 * run. HAZARD (RX capture): the call leaves the engine enabled and armed
 * (CTRL 0x281, EN2 9, BD0 0x101, START/END at the DSP buffer).
 *                                                         [static] [measured] */

#endif /* HW_MT7996_DFE_H */
