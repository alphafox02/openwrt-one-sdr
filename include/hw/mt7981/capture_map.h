/* SPDX-License-Identifier: MIT */
/*
 * MT7981 (OpenWrt One) IQ-capture path — register map CONFIRMED ON HARDWARE.
 *
 * The MT7976 A-die capture engine on the MT7981 is register-for-register
 * identical to the MT7996's (../mt7996/icap.h, cap_route.h, dfe.h, phy.h) and
 * sits at the SAME WF-bus addresses. Every block below is reachable from the
 * host via the stock mt76 debugfs regidx/regval window (write the address to
 * regidx, read/write the 32-bit value via regval) — no driver or firmware
 * change needed to touch the registers.
 *
 * Confirmed on hardware: all ICAP0 CR reset values
 * matched ../mt7996/icap.h exactly (CR0=0x400, CR2=0x100000, CR3=0x11fffc,
 * CR5=0x13fffc, CR12=0xa8027fff); host writes to ICAP CR2/CR3 and route +0x24
 * read back; a host-driven arm made the engine strobe (CR10=0x7ffc).
 */
#ifndef HW_MT7981_CAPTURE_MAP_H
#define HW_MT7981_CAPTURE_MAP_H

/* ICAP writers: ICAPn = base + n*0x1000, CRk at +0x90 + 4k (see mt7996/icap.h). */
#define MT7981_ICAP_BASE(n)	(0x80023000u + 0x1000u * (unsigned int)(n))
#define MT7981_ICAP_CR(n, k)	(MT7981_ICAP_BASE(n) + 0x90u + 4u * (unsigned int)(k))
/* CR0 control words (mt7996/icap.h): IDLE 0x400, ARM 0x401, TRIGGER 0x403,
 * RING_IDLE 0x2400, RING_ARM 0x2401, RING_TRIGGER 0x2403. Window = CR2 0,
 * CR3 0xffc (1024 beats). CR9 = ring write-pointer (poll). CR10 = packers. */

/* Capture route block (mt7996/cap_route.h). HOST-WRITABLE.
 * HAZARD: never write +0x00, +0x04[23:0] or +0x08 (hang the MCU). */
#define MT7981_CAP_ROUTE(ofs)	(0x88009000u + (unsigned int)(ofs))
/* Window/ICAP0 route (hold across trigger, release with +0x04=0):
 *   +0x04 = 0xff000000, +0x0c = 0, +0x24 = 0x02000000, +0x28 = 0x400 */

/* PHY + DFE capture mux (mt7996/phy.h, dfe.h), band b at + b<<20. The WM uses
 * this 0x830xxxxx space; host-accessible via regidx. */
#define MT7981_PHY_BASE		0x83000000u
#define MT7981_PHY_BAND_STRIDE	0x00100000u
#define MT7981_DFE_CAP(b, ofs)	(0x830ad000u + (unsigned int)(b) * MT7981_PHY_BAND_STRIDE + (unsigned int)(ofs))
#define MT7981_DFE_CAP_MODE(b)	MT7981_DFE_CAP(b, 0x440)	/* [7:0] activation, stock node 0x11 */
#define MT7981_DFE_CAP_MUX(b)	MT7981_DFE_CAP(b, 0x448)	/* SEL0[7:0]=0x14, RATE0[17:16], ACT0[21] */
#define MT7981_DFE_CAP_CTRL(b)	MT7981_DFE_CAP(b, 0x460)	/* EN = bit 1 */
#define MT7981_PHY_BN_CLK0(b)	(MT7981_PHY_BASE + (unsigned int)(b) * MT7981_PHY_BAND_STRIDE + 0x80004u) /* |= bit25 CAPTURE */
#define MT7981_PHY_BN_CLK1(b)	(MT7981_PHY_BASE + (unsigned int)(b) * MT7981_PHY_BAND_STRIDE + 0x80008u)

/* Landing memory (host-readable). */
#define MT7981_SYSRAM_WINDOW	0x00464000u	/* 16 KiB window; layout in mt7996/iq_format.h */
#define MT7981_SYSRAM_WINDOW_SIZE 0x00004000u
#define MT7981_UMAC_POOL	0xe0400000u	/* ICAP0 ring/stream pool */

/* STILL TO DO for samples to land (the "prime", mt7996/capture_setup.h): DFE
 * CTRL|=EN + MODE + MUX, PHY CAPTRIG_CFG + capture trigger, BN_CLK|=CAPTURE,
 * and an ACTIVE RX (band tuned + receiving). Then arm ICAP and read the window. */

#endif /* HW_MT7981_CAPTURE_MAP_H */
