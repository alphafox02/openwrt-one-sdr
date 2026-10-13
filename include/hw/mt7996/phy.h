/* SPDX-License-Identifier: MIT */
/*
 * MT7996 PHY (baseband, "D-die" digital side): per-chain RX gain/AGC blocks,
 * per-chain RF-interface (RFINTF) and TSSI blocks (with the RX IQ
 * compensation filter bank), the per-band RX/TX control block
 * (PHYRX_CTRL_BN + PHYDFE_CTRL_BN, including the digital RX front end
 * WF_RXFE_01), the per-band WF_COMM block (CSI, radar/RDD, AHB bridge, TOAE
 * accelerator, background receiver, the TTG RX DC and IQ engines), DFS, the
 * band -> PHY -> A-die map, the per-band PHY init of the calibration init
 * and the RX front-end words of the channel set (the AGC table port, the
 * per-chain RX gain offsets; its order is in phy_cal.h).
 *
 * ADDRESS SPACE: WF bus, as the WM sees it (0x83000000..0x832fffff). All
 * registers are 32-bit, word access only.
 *   - WM/WA: plain loads/stores. The WM runs once for all bands and adds
 *     band << 20 itself.
 *   - DSP cores: one DSP per band; each sees ITS OWN band's PHY at the band-0
 *     address (no band offset). [static]
 *   - Host: the HIF L2 remap (page selector BAR0+0x1b4 [19:0] = addr >> 12,
 *     data window BAR0+0x1000 + (addr & 0xfff)); see the host PCIe headers.
 *     Or the firmware REG_ACCESS command (UNI cid 0x0d): the stock WM serves
 *     [0x70000000, 0xe0000000), which covers this space (see mcu_dic.h).
 *
 * ADDRESS FORMULA [static, measured on bands 0..2]:
 *   addr = 0x83000000 + band << 20 + block + offset
 *   per-chain blocks: block = chain << 16 + sub-block (sub-block 0x0000 CSD,
 *     0x7000 RFINTF, 0xa000 DPD LUTs, 0xc000 TSSI, 0xd000, 0xe000, 0xf000)
 *   per-band blocks: 0x80000 PHYRX_CTRL_BN / PHYDFE_CTRL_BN, 0x90000 twin,
 *     0xa0000 WF_COMM (DFS at +0xf000 for band 0; band 1's DFS resolves to
 *     0x830bf000, see MT7996_PHY_DFS_BASE)
 * The chain index occupies bits [19:16]; chain position 4 is 0x83040000 +
 * band << 20: on band 0 the monitor receiver (see below), on band 2 the
 * fifth 6 GHz RX chain.
 * Block-level map with ROM module ids: phy_map.h. Calibration and TX power
 * (TSSI/TPC) registers: phy_cal.h. DFE capture mux, lane mux and DFE
 * calibration capture engine (0x83xad440.., 0x83xad594.., 0x8300e81c): dfe.h.
 * DSP trigger/mirror words (0x8308ef04.., 0x830a02b4..): dsp.h. A-die
 * registers: the adie/ headers. CONN AFE ADC-stop word: conn.h.
 *
 * BAND / PHY / A-DIE MAP [static; LO and chip ids measured]:
 *   band  RF band   A-die (SPI ant id base)  RX chains  mt76 RX chain mask
 *   0     2.4 GHz   die 0, MT7976 (0x00)     4 (+4)     0x000f
 *   1     5 GHz     die 1, MT7977 (0x10)     4          0x00f0
 *   2     6 GHz     die 2, MT7977 (0x20)     5          0x1f00
 *   Band index = PHY index = DBDC band index = A-die index: the WM adds
 *   0x100000 when either band or PHY index is 1, 0x200000 when 2. A-die
 *   antenna ids are (die << 4) | wf, wf 0..4 (ROM names WF0..4 = 0x00..0x04,
 *   SEC_WF0..4 = 0x10..0x14, THR_WF0..4 = 0x20..0x24).
 *   The LMAC blocks of the same band are at 0x820e0000 / 0x820f0000 /
 *   0x830e0000 (band 0/1/2); not described here.
 *   Band 0 chain 4 is die 0's WF4 "DFS/SM" monitor receiver (radar/RDD 2,
 *   off-channel scan); it is not a normal RX chain and its IQ has not been
 *   captured. [static; capture attempts measured negative]
 *
 * RX CHAIN MASKS [static]: mt76 builds the global RX chain mask from the
 * EEPROM TX path count, plus the next bit when the RX path count differs
 * (band 2: 5 RX, 4 TX), and shifts it to the band-local position for firmware
 * channel commands (UNI_CHANNEL_SWITCH carries the bit count instead).
 * Under a single-chain RX mask every capture tap carries that chain in
 * stream 0. [measured]
 * HAZARD: a firmware channel switch with band-0 RX mask 0x10 (chain 4 alone)
 * crashes the WM; recovery is a reload. [measured]
 *
 * HAZARD: host BAR reads in the WFSYS domain (0x820c0000..0x830fffff, which
 * includes most of this space) have hard-locked the host and left the MT7996
 * wedged until power was removed; FLR, secondary bus reset and PERST# do not
 * clear it. Make the first touch of an address a read, with the firmware
 * running, under a watchdog; prefer REG_ACCESS or WM-side code. [measured on
 * other WFSYS addresses; 0x830a1000 was read safely through the L2 remap]
 * HAZARD: a radio reload / firmware bring-up transmits a full-scale burst on
 * the antenna ports; disconnect cabled instruments first. [measured]
 *
 * Confidence tags: [measured] verified on hardware, [static] read from
 * firmware/ROM/driver code or ROM register tables, [inferred].
 */
#ifndef HW_MT7996_PHY_H
#define HW_MT7996_PHY_H

#include "../hw_bits.h"

/* ========================================================================
 * Address formula, bands, chains
 * ======================================================================== */

#define MT7996_PHY_BASE                   0x83000000u
#define MT7996_PHY_BAND_STRIDE            0x00100000u	/* band << 20 */
#define MT7996_PHY_CHAIN_STRIDE           0x00010000u	/* chain << 16 */

/* Per-chain register: block offset 'off' in chain 'chain' of band 'band'. */
#define MT7996_PHY_CHAIN_REG(band, chain, off) \
	(0x83000000u + ((band) << 20) + ((chain) << 16) + (off))
/* Per-band register: 'off' is relative to the band base (0x83000000 + band<<20),
 * e.g. 0x80014 for RST_CTRL. */
#define MT7996_PHY_BAND_REG(band, off) \
	(0x83000000u + ((band) << 20) + (off))

#define MT7996_PHY_NUM_BANDS              3u
#define MT7996_PHY_BAND_2G4               0u	/* die 0, MT7976 */
#define MT7996_PHY_BAND_5G                1u	/* die 1, MT7977 */
#define MT7996_PHY_BAND_6G                2u	/* die 2, MT7977 */
/* RX chains carrying antenna data: 4/4/5. [static: EEPROM 4/4/5 RX paths] */
#define MT7996_PHY_BAND_RX_CHAINS(band)   ((band) == 2 ? 5u : 4u)
/* Chain 4 of band 0: the monitor (DFS/SM) receiver, die 0 WF4. [static] */
#define MT7996_PHY_MONITOR_CHAIN          4u
/* A-die serving a band, and its SPI antenna id for chain 'wf'. [measured] */
#define MT7996_PHY_BAND_ADIE(band)        (band)
#define MT7996_PHY_ADIE_ANT(band, wf)     ((((band) & 0xfu) << 4) | ((wf) & 0xfu))
/* mt76 global RX chain masks with the full EEPROM profile. [static] */
#define MT7996_PHY_RX_CHAINMASK_B0        0x000fu
#define MT7996_PHY_RX_CHAINMASK_B1        0x00f0u
#define MT7996_PHY_RX_CHAINMASK_B2        0x1f00u

/* ========================================================================
 * Per-chain CSD block (vendor PHYRX_CSD): RX gain force, AGC, gain readback
 * 0x83000000 + band<<20 + chain<<16 + 0x000..0x3fc
 *
 * The force register is a plain MMIO word; no firmware command or A-die SPI
 * is needed to hold the receive gain. Changing it mid-stream acts on the
 * captured samples within the stream. [measured]
 * No global AGC enable/freeze bit is known; only the per-chain forces. [static]
 * ======================================================================== */

#define MT7996_PHY_CSD(band, chain, off)  MT7996_PHY_CHAIN_REG(band, chain, off)

/*
 * RXGAIN (vendor AGC_311), +0xc4, RW, reset/AGC value 0. [measured]
 * Three force sets, each = one enable bit in [2:0], a 3-bit LNA index and a
 * 4-bit LPF index, eight bits apart. Write 0 to hand gain back to the AGC.
 *   Set A [0]/[10:8]/[15:12]: vendor names FORCE_GAIN_IDX / FORCE_LNA_IDX_5LVL
 *     / FORCE_LPF_IDX. Drives the captured path. [measured]
 *   Set B [1]/[18:16]/[23:20]: the vendor's ICAP "user RX gain" knob. Drives
 *     the captured path identically to set A. [measured]
 *   Set C [2]/[26:24]/[31:28]: used by iBF phase and RX-gain calibration; has
 *     NO effect on the captured path. [measured]
 * Measured steps (CW tone, either of A/B forced, both indexes held):
 *   LNA 4->3 -5.6 dB, 3->2 -5.8 dB, 2->1 -6.1 dB, 1->0 -6.8 dB (~24 dB total).
 *   LPF 9->1: ~2 dB per step (~16 dB total); index 0 does not follow the
 *   2 dB slope. Highest gain of the indexes measured: LNA 4, LPF 9.
 *   [measured]
 * Values the vendor programs: LNA 0..5 (5 only in set C), LPF 0, 3, 5, 7, 9.
 * The vendor ICAP recipe writes set-B words 0x00940002, 0x00930002,
 * 0x00920002, 0x00910002, 0x00900002, 0x00500002, 0x00000002 = (LNA,LPF)
 * (4,9) (3,9) (2,9) (1,9) (0,9) (0,5) (0,0), holds the force from before the
 * capture trigger to 50 us after it, then writes 0. [static]
 * Bits [7:3], [11], [19], [27] are set by no code read; the vendor
 * recipe writes [19:16] as a nibble with [19] = 0, calibration code masks
 * [18:16]. [static]
 * The TTG RX calibrations (phy_cal.h T1{18..23}) set [2], then [1], then
 * [0] on every chain, keeping the index fields (MediaTek's cold load:
 * 0xf4000000 -> 0xf4000007). [static][measured]
 */
#define MT7996_PHY_RXGAIN(band, chain)    MT7996_PHY_CSD(band, chain, 0xc4u)
#define   MT7996_PHY_RXGAIN_A_EN          HW_BIT(0)
#define   MT7996_PHY_RXGAIN_B_EN          HW_BIT(1)
#define   MT7996_PHY_RXGAIN_C_EN          HW_BIT(2)
#define   MT7996_PHY_RXGAIN_A_LNA         HW_GENMASK(10, 8)
#define   MT7996_PHY_RXGAIN_A_LPF         HW_GENMASK(15, 12)
#define   MT7996_PHY_RXGAIN_B_LNA         HW_GENMASK(18, 16)
#define   MT7996_PHY_RXGAIN_B_LPF         HW_GENMASK(23, 20)
#define   MT7996_PHY_RXGAIN_C_LNA         HW_GENMASK(26, 24)
#define   MT7996_PHY_RXGAIN_C_LPF         HW_GENMASK(31, 28)
#define   MT7996_PHY_RXGAIN_AGC           0x00000000u	/* all forces off */
/* LNA index (vendor ENUM_LNA_GAIN_TYPE, 5 levels). [static; order measured] */
#define   MT7996_PHY_RXGAIN_LNA_UL        0u	/* ultra-low, lowest gain */
#define   MT7996_PHY_RXGAIN_LNA_L         1u
#define   MT7996_PHY_RXGAIN_LNA_M         2u
#define   MT7996_PHY_RXGAIN_LNA_H         3u
#define   MT7996_PHY_RXGAIN_LNA_UH        4u	/* ultra-high, highest gain */
#define   MT7996_PHY_RXGAIN_LPF_MAX       9u	/* highest index any code uses */
/* Sets A and B forced together to (lna, lpf): the word this project uses. */
#define MT7996_PHY_RXGAIN_FORCE_AB(lna, lpf) \
	(MT7996_PHY_RXGAIN_A_EN | HW_PREP(MT7996_PHY_RXGAIN_A_LNA, lna) | \
	 HW_PREP(MT7996_PHY_RXGAIN_A_LPF, lpf) | MT7996_PHY_RXGAIN_B_EN | \
	 HW_PREP(MT7996_PHY_RXGAIN_B_LNA, lna) | HW_PREP(MT7996_PHY_RXGAIN_B_LPF, lpf))

/* CSD +0xc8: [23:16] retuned by the firmware's SW-LNA AGC profiles. [static] */
#define MT7996_PHY_CSD_C8(band, chain)    MT7996_PHY_CSD(band, chain, 0xc8u)
#define   MT7996_PHY_CSD_C8_SWLNA         HW_GENMASK(23, 16)

/*
 * AGC_14, +0xd0, RW: coarse-AGC switch force. [static: ROM names on chain
 * 0x83040000; the radar chain sets EN = 1, STATE = 4]
 * The per-band PHY init's register table writes 0x40234000 on chains 0..4
 * of every band (EN = 0; the ROM's AGC_14 on 0x83040000 is position 4 of
 * band 0). [static][measured]
 */
#define MT7996_PHY_AGC_CAGC(band, chain)  MT7996_PHY_CSD(band, chain, 0xd0u)
#define   MT7996_PHY_AGC_CAGC_SW_EN       HW_BIT(27)	/* FORCE_CAGC_SWITCH_EN */
#define   MT7996_PHY_AGC_CAGC_SW_STATE    HW_GENMASK(26, 24)	/* FORCE_CAGC_SWITCH_STATE */
#define   MT7996_PHY_AGC_CAGC_INIT        0x40234000u

/* CSD +0xd4: [19:14] retuned by SW-LNA profiles. [static] */
#define MT7996_PHY_CSD_D4(band, chain)    MT7996_PHY_CSD(band, chain, 0xd4u)
#define   MT7996_PHY_CSD_D4_SWLNA         HW_GENMASK(19, 14)

/* CSD +0xdc: AGC table port [name guess]. Every channel set (V:e00d1a5e,
 * phy_cal.h "Channel set") writes whole words entry by entry: 50 entries
 * per chain on band 0 (chains 0..3), 48 on bands 1 and 2 (chains 0..4),
 * index 0..49 / 0..47 in [21:16] with [31] set and the entry in the low
 * half (e.g. 0x800004e3, 0x800104e4, ...; increasing within groups of 10 /
 * 8). Values from u32 tables in WM .data: 2.4 GHz 0x022197d4 (stride 8), 5
 * / 6 GHz 0x02219970 (stride 0x14; band 1 (BW160) reads column 0, band 2
 * (BW320) column 1). Part of the receive-only subset. [static][measured]
 * [inferred: the field split, from the values written] */
#define MT7996_PHY_CSD_AGC_TBL(band, chain) MT7996_PHY_CSD(band, chain, 0xdcu)
#define   MT7996_PHY_CSD_AGC_TBL_WR       HW_BIT(31)
#define   MT7996_PHY_CSD_AGC_TBL_IDX      HW_GENMASK(21, 16)
#define   MT7996_PHY_CSD_AGC_TBL_VAL      HW_GENMASK(15, 0)
#define MT7996_PHY_CSD_AGC_TBL_N_2G4      50u
#define MT7996_PHY_CSD_AGC_TBL_N_5G6G     48u

/* CSD +0x000 [19:0] and +0x104 [30:12]: retuned by SW-LNA profiles. [static] */
#define MT7996_PHY_CSD_00(band, chain)    MT7996_PHY_CSD(band, chain, 0x000u)
#define   MT7996_PHY_CSD_00_SWLNA         HW_GENMASK(19, 0)
#define MT7996_PHY_CSD_104(band, chain)   MT7996_PHY_CSD(band, chain, 0x104u)
#define   MT7996_PHY_CSD_104_SWLNA        HW_GENMASK(30, 12)

/*
 * AGC_28NMRF_0, +0x108, RW: 6-bit RF/digital gain words that accompany the
 * forces. [29:24] FORCE_RFDGC_GAIN (set A, named on chain 0x83040000; the
 * radar chain writes 0). [21:16] a 6-bit gain written with set C by iBF
 * calibration (values 0x01..0x3f). Meaning of the gain units not decoded.
 * [static]
 */
#define MT7996_PHY_AGC_RFDGC(band, chain) MT7996_PHY_CSD(band, chain, 0x108u)
#define   MT7996_PHY_AGC_RFDGC_A_GAIN     HW_GENMASK(29, 24)
#define   MT7996_PHY_AGC_RFDGC_C_GAIN     HW_GENMASK(21, 16)

/* CSD +0x120 [6:0], +0x130 [6:0]: written on all band-0/1 chains by an AGC
 * setup routine and by SW-LNA profiles; AGC thresholds. [static; meaning
 * inferred] A manual-gain recipe leaves them alone. */
#define MT7996_PHY_AGC_TH0(band, chain)   MT7996_PHY_CSD(band, chain, 0x120u)
#define MT7996_PHY_AGC_TH1(band, chain)   MT7996_PHY_CSD(band, chain, 0x130u)
#define   MT7996_PHY_AGC_TH_VAL           HW_GENMASK(6, 0)

/*
 * RXGAIN_D, +0x150, RW: fourth force set. The per-band PHY init's register
 * table writes 0x0040f0fa on chains 0..4 of every band at calibration init
 * ([31] = 0, [22:20] = 4, [15:12] = 0xf) [static][measured]; RX-gain
 * calibration then sets [31], clears [15:12] and writes [22:20] = the LNA
 * index under calibration [static]. Effect on the captured path not
 * measured.
 */
#define MT7996_PHY_RXGAIN_D(band, chain)  MT7996_PHY_CSD(band, chain, 0x150u)
#define   MT7996_PHY_RXGAIN_D_EN          HW_BIT(31)
#define   MT7996_PHY_RXGAIN_D_LNA         HW_GENMASK(22, 20)
#define   MT7996_PHY_RXGAIN_D_15_12       HW_GENMASK(15, 12)	/* cleared by cal */
#define   MT7996_PHY_RXGAIN_D_INIT        0x0040f0fau

/* CSD +0x154: [28:24] cleared on chains 0..4 by WM PHY preparation and by a
 * calibration. Not decoded. [static] */
#define MT7996_PHY_CSD_154(band, chain)   MT7996_PHY_CSD(band, chain, 0x154u)
#define   MT7996_PHY_CSD_154_28_24        HW_GENMASK(28, 24)

/* CSD +0x180 [4:0], +0x1b4 [13:8]/[21:16], +0x1bc [13:8], +0x1c0
 * [13:8]/[21:16]: retuned by SW-LNA profiles (AGC gain thresholds).
 * [static; meaning inferred] */
#define MT7996_PHY_CSD_180(band, chain)   MT7996_PHY_CSD(band, chain, 0x180u)
#define MT7996_PHY_CSD_1B4(band, chain)   MT7996_PHY_CSD(band, chain, 0x1b4u)
#define MT7996_PHY_CSD_1BC(band, chain)   MT7996_PHY_CSD(band, chain, 0x1bcu)
#define MT7996_PHY_CSD_1C0(band, chain)   MT7996_PHY_CSD(band, chain, 0x1c0u)
#define   MT7996_PHY_CSD_SWLNA_LO         HW_GENMASK(13, 8)
#define   MT7996_PHY_CSD_SWLNA_HI         HW_GENMASK(21, 16)

/*
 * Per-chain RX gain offsets [name guess], written by the second
 * channel-set pass of V:e00d2540 (flag 0, after the T2 calibrations;
 * phy_cal.h "Channel set" step 16) on the band's WFs (0..3; band 2 0..4)
 * as read-modify-writes of words PHY table B writes earlier in the same
 * channel set (phy_map.h), from the WF's signed RX gain EEPROM bytes (E(x, y),
 * phy_cal.h MT7996_PHY_FW_GAINSTEP_IN: base 0x65e 2.4 GHz, 0x66e 5 GHz,
 * 0x6aa 6 GHz) and the WM u32 tables 0xe0151250..0xe01512ec
 * (V:e00d2048..V:e00d216c): [static][measured]
 *   +0x188: four 7-bit fields [30:24], [22:16], [14:8], [6:0] ([31],
 *           [23], [15], [7] kept); stock band 1 0x0b11181e -> 0x0d13181e;
 *   +0x0f8 [30:24];
 *   +0x164, +0x160: [24:16] and [8:0] (9-bit);
 *   +0x390 [30:24] and +0x408 [24:16] (slot 5): only when the channel
 *           group is not 0 (5 / 6 GHz: bands 1 and 2 here, not band 0).
 */
#define MT7996_PHY_CSD_RXGOFS_188(band, chain) MT7996_PHY_CSD(band, chain, 0x188u)
#define   MT7996_PHY_CSD_RXGOFS_188_F3    HW_GENMASK(30, 24)
#define   MT7996_PHY_CSD_RXGOFS_188_F2    HW_GENMASK(22, 16)
#define   MT7996_PHY_CSD_RXGOFS_188_F1    HW_GENMASK(14, 8)
#define   MT7996_PHY_CSD_RXGOFS_188_F0    HW_GENMASK(6, 0)
#define   MT7996_PHY_CSD_RXGOFS_188_KEEP  (HW_BIT(31) | HW_BIT(23) | HW_BIT(15) | HW_BIT(7))
#define MT7996_PHY_CSD_RXGOFS_0F8(band, chain) MT7996_PHY_CSD(band, chain, 0x0f8u)
#define MT7996_PHY_CSD_RXGOFS_390(band, chain) MT7996_PHY_CSD(band, chain, 0x390u)
#define   MT7996_PHY_CSD_RXGOFS_HI7       HW_GENMASK(30, 24)	/* +0x0f8, +0x390 */
#define MT7996_PHY_CSD_RXGOFS_164(band, chain) MT7996_PHY_CSD(band, chain, 0x164u)
#define MT7996_PHY_CSD_RXGOFS_160(band, chain) MT7996_PHY_CSD(band, chain, 0x160u)
#define MT7996_PHY_CSD_RXGOFS_408(band, chain) MT7996_PHY_CSD(band, chain, 0x408u)
#define   MT7996_PHY_CSD_RXGOFS_HI9       HW_GENMASK(24, 16)	/* +0x164, +0x160, +0x408 */
#define   MT7996_PHY_CSD_RXGOFS_LO9       HW_GENMASK(8, 0)	/* +0x164, +0x160 */

/*
 * AGC status readbacks, RO. +0x3f8 is read on every chain by the WM and split
 * into bytes [31:24], [23:16], [15:8]; +0x3e0 and +0x3ec are read by sibling
 * getters, +0x3e0 is zeroed on two chains by firmware (so not purely RO).
 * Gain/RSSI status. [static; meaning inferred]
 */
#define MT7996_PHY_AGC_STAT0(band, chain) MT7996_PHY_CSD(band, chain, 0x3e0u)
#define MT7996_PHY_AGC_STAT1(band, chain) MT7996_PHY_CSD(band, chain, 0x3ecu)
#define MT7996_PHY_AGC_STAT2(band, chain) MT7996_PHY_CSD(band, chain, 0x3f8u)
#define   MT7996_PHY_AGC_STAT2_B3         HW_GENMASK(31, 24)
#define   MT7996_PHY_AGC_STAT2_B2         HW_GENMASK(23, 16)
#define   MT7996_PHY_AGC_STAT2_B1         HW_GENMASK(15, 8)

/* IRPI (in-band power histogram) per chain, +0x1000; cleared through
 * RXTD12 (below). [static: mt76] */
#define MT7996_PHY_IRPI(band, chain)      MT7996_PHY_CSD(band, chain, 0x1000u)

/* Offset names, role unknown unless a header says more: rows of the per-band PHY init's
 * register tables (phy_init.h MT7996_PHY_INIT_REGTBL_*; "init", written whole on every
 * band, never read by the init) and of the channel-set PHY tables A / B (phy_chan.h
 * MT7996_CHSET_TBL_A_ROWS / _B_ROWS; "table A" / "table B"), and words the channel set
 * writes by field. [static][measured]
 * CSD sub-block: RX gain force, AGC, CAGC. */
#define MT7996_PHY_CSD_004(band, chain)    MT7996_PHY_CSD(band, chain, 0x004u)	/* table B */
#define MT7996_PHY_CSD_008(band, chain)    MT7996_PHY_CSD(band, chain, 0x008u)	/* table B */
#define MT7996_PHY_CSD_014(band, chain)    MT7996_PHY_CSD(band, chain, 0x014u)	/* table B */
#define MT7996_PHY_CSD_024(band, chain)    MT7996_PHY_CSD(band, chain, 0x024u)	/* table B */
#define MT7996_PHY_CSD_028(band, chain)    MT7996_PHY_CSD(band, chain, 0x028u)	/* table B */
#define MT7996_PHY_CSD_034(band, chain)    MT7996_PHY_CSD(band, chain, 0x034u)	/* table B */
#define MT7996_PHY_CSD_038(band, chain)    MT7996_PHY_CSD(band, chain, 0x038u)	/* table B */
#define MT7996_PHY_CSD_054(band, chain)    MT7996_PHY_CSD(band, chain, 0x054u)	/* table B */
#define MT7996_PHY_CSD_058(band, chain)    MT7996_PHY_CSD(band, chain, 0x058u)	/* table B */
#define MT7996_PHY_CSD_0EC(band, chain)    MT7996_PHY_CSD(band, chain, 0x0ecu)	/* table B */
#define MT7996_PHY_CSD_0F4(band, chain)    MT7996_PHY_CSD(band, chain, 0x0f4u)	/* init */
#define MT7996_PHY_CSD_0FC(band, chain)    MT7996_PHY_CSD(band, chain, 0x0fcu)	/* table B */
#define MT7996_PHY_CSD_10C(band, chain)    MT7996_PHY_CSD(band, chain, 0x10cu)	/* table B */
#define MT7996_PHY_CSD_11C(band, chain)    MT7996_PHY_CSD(band, chain, 0x11cu)	/* table B */
#define MT7996_PHY_CSD_124(band, chain)    MT7996_PHY_CSD(band, chain, 0x124u)	/* table B */
#define MT7996_PHY_CSD_13C(band, chain)    MT7996_PHY_CSD(band, chain, 0x13cu)	/* init */
#define MT7996_PHY_CSD_140(band, chain)    MT7996_PHY_CSD(band, chain, 0x140u)	/* table B */
#define MT7996_PHY_CSD_144(band, chain)    MT7996_PHY_CSD(band, chain, 0x144u)	/* init */
#define MT7996_PHY_CSD_148(band, chain)    MT7996_PHY_CSD(band, chain, 0x148u)	/* table B */
#define MT7996_PHY_CSD_14C(band, chain)    MT7996_PHY_CSD(band, chain, 0x14cu)	/* table B */
#define MT7996_PHY_CSD_16C(band, chain)    MT7996_PHY_CSD(band, chain, 0x16cu)	/* table B */
#define MT7996_PHY_CSD_198(band, chain)    MT7996_PHY_CSD(band, chain, 0x198u)	/* init */
#define MT7996_PHY_CSD_1B8(band, chain)    MT7996_PHY_CSD(band, chain, 0x1b8u)	/* table B */
#define MT7996_PHY_CSD_1C4(band, chain)    MT7996_PHY_CSD(band, chain, 0x1c4u)	/* table B */
#define MT7996_PHY_CSD_1D0(band, chain)    MT7996_PHY_CSD(band, chain, 0x1d0u)	/* table B */
#define MT7996_PHY_CSD_1DC(band, chain)    MT7996_PHY_CSD(band, chain, 0x1dcu)	/* table B */
#define MT7996_PHY_CSD_1E0(band, chain)    MT7996_PHY_CSD(band, chain, 0x1e0u)	/* table B */
#define MT7996_PHY_CSD_1E4(band, chain)    MT7996_PHY_CSD(band, chain, 0x1e4u)	/* init */
#define MT7996_PHY_CSD_1E8(band, chain)    MT7996_PHY_CSD(band, chain, 0x1e8u)	/* init */
#define MT7996_PHY_CSD_1F0(band, chain)    MT7996_PHY_CSD(band, chain, 0x1f0u)	/* init */
#define MT7996_PHY_CSD_1FC(band, chain)    MT7996_PHY_CSD(band, chain, 0x1fcu)	/* table B */
#define MT7996_PHY_CSD_230(band, chain)    MT7996_PHY_CSD(band, chain, 0x230u)	/* table B */
#define MT7996_PHY_CSD_23C(band, chain)    MT7996_PHY_CSD(band, chain, 0x23cu)	/* table B */
#define MT7996_PHY_CSD_244(band, chain)    MT7996_PHY_CSD(band, chain, 0x244u)	/* table B */
#define MT7996_PHY_CSD_248(band, chain)    MT7996_PHY_CSD(band, chain, 0x248u)	/* table B */
#define MT7996_PHY_CSD_250(band, chain)    MT7996_PHY_CSD(band, chain, 0x250u)	/* init */
#define MT7996_PHY_CSD_280(band, chain)    MT7996_PHY_CSD(band, chain, 0x280u)	/* init */
#define MT7996_PHY_CSD_400(band, chain)    MT7996_PHY_CSD(band, chain, 0x400u)	/* table B */

/* ========================================================================
 * Per-chain RFINTF block (vendor PHYDFE_RFINTF_WFn_CMM, ROM modules
 * 0x4b..0x52, no ROM table): 0x83007000 + band<<20 + chain<<16.
 * The interface between the A-die analog I/Q and the D-die RX front end.
 * Only the fields below are known; the WM holds ~60 address constants into
 * this block with no names. [static] The manual TX gain (+0x250), the TSSI
 * measurement words (+0x300, +0x304, +0x308, +0x3a0, +0x3b0) of the
 * channel-set TX calibrations, the TX gain-step calibration and the TX LO /
 * IQ calibration's power meter, and that calibration's engine status
 * (+0x394) are in phy_cal.h; the TX LO-leakage and TX IQ codes
 * (+0x600..+0x69c), the TX gain-step code table (+0x6b0..+0x6b8) and the TX
 * IQ calibration's rows (+0x6bc..+0x7d8) are below. The TTG-tone RX
 * IQ calibrations'
 * words (+0x240..+0x3e8) and their two result tables, the per-gain RX IQ
 * table (+0x820..+0x868) and the RX IQ compensation filter bank
 * (+0x8d0..+0xc8c), are below (phy_cal.h mask bits 15, 16), with the
 * calibration failure log that overlaps the bank (+0xa14..+0xa28). Offsets
 * in the text are RFINTF-relative (+0x258 = chain +0x7258).
 * ======================================================================== */

#define MT7996_PHY_RFINTF(band, chain, off) MT7996_PHY_CHAIN_REG(band, chain, 0x7000u + (off))

/* RFINTF +0x240: per-chain calibration routing. The TTG RX calibration
 * writes [29:28] = chain (1..3; 0 for chains 0 and 4), [19] = (chain >= 4),
 * [1:0] = 1 (V:e0049472: [0] = 1, [1] = 0); the DC engine holds [1] = 1
 * while it runs on the chain (V:e00498fc); T1[23]'s IQ phase sets [1:0] = 2
 * (V:e004b48a, from P[9]: 0xff -> 2, 0xfe -> 1, 0xfd -> 0). The TX IQ
 * calibration (phy_cal.h T1{8,12,16}) writes chain 0's [29:28] = its
 * loopback receiver chain (MT7996_PHY_TXIQ_RX_CHAIN), whatever the WF.
 * [static][measured] */
#define MT7996_PHY_RFINTF_CALSEL(band, chain) MT7996_PHY_RFINTF(band, chain, 0x240u)
#define   MT7996_PHY_RFINTF_CALSEL_CHAIN  HW_GENMASK(29, 28)	/* [guess: chain select] */
#define   MT7996_PHY_RFINTF_CALSEL_HI     HW_BIT(19)
#define   MT7996_PHY_RFINTF_CALSEL_MODE   HW_GENMASK(1, 0)	/* [guess: name] */

/*
 * TRX_CLK, RFINTF +0x254, RW: TX-DAC / RX-ADC clock enables. ROM
 * halToaeSetTRxClk writes [5:4] both = TX-DAC on, [7:6] both = RX-ADC on,
 * at 0x83007254 + (its PHY argument << 16), no band << 20 term. The WM PHY
 * register helper sets [7] then [6] on chains 0..4, then [5]/[4] by mode.
 * The TX IQ calibration (phy_cal.h T1{8,12,16}) sets [7] on chains 0..3 and
 * [6] on its loopback receiver chain. Chooses no frequency. [static]
 */
#define MT7996_PHY_TRX_CLK(band, chain)   MT7996_PHY_RFINTF(band, chain, 0x254u)
#define   MT7996_PHY_TRX_CLK_TX_DAC_ON    HW_GENMASK(5, 4)
#define   MT7996_PHY_TRX_CLK_RX_ADC_ON    HW_GENMASK(7, 6)

/* RFINTF +0x258: TTG RX setup [12] = 1, [11:8] = 0, [7] = 1, [4:0] = 0
 * (once per call; mode 0x16 again every step). Observed 0.
 * [static][measured] */
#define MT7996_PHY_RFINTF_7258(band, chain) MT7996_PHY_RFINTF(band, chain, 0x258u)
/* RFINTF +0x268 / +0x26c / +0x28c: written by the per-band PHY init (below)
 * on chains 0..4 of every band. +0x268: [19:0] = 0, [31:28] = 0 (chains
 * 0..3) or 0xf (chain 4) (V:e00dec2a..V:e00dec86); reads 0x00080808 before
 * (chains 0..3 of every band, band-2 chain 4), 0 on band 0 / band 1 chain 4.
 * +0x26c: [7:0] = 0; reads 0xf0 before (chains 0..3, band-2 chain 4), 0 on
 * band 0 / 1 chain 4. +0x28c: whole word 0x44664447 (write only). Roles
 * unknown [guess: +0x268 chain 4 [31:28] = 0xf parks the unused fifth
 * position; +0x28c nibble-coded per-path settings]. [static][measured] */
#define MT7996_PHY_RFINTF_7268(band, chain) MT7996_PHY_RFINTF(band, chain, 0x268u)
#define   MT7996_PHY_RFINTF_7268_LO       HW_GENMASK(19, 0)
#define   MT7996_PHY_RFINTF_7268_HI       HW_GENMASK(31, 28)
#define   MT7996_PHY_RFINTF_7268_HI_CH4   0xfu	/* e00dec76: chain 4; chains 0..3: 0 */
#define MT7996_PHY_RFINTF_726C(band, chain) MT7996_PHY_RFINTF(band, chain, 0x26cu)
#define   MT7996_PHY_RFINTF_726C_LO       HW_GENMASK(7, 0)
#define MT7996_PHY_RFINTF_728C(band, chain) MT7996_PHY_RFINTF(band, chain, 0x28cu)
#define   MT7996_PHY_RFINTF_728C_INIT     0x44664447u
/* RFINTF +0x2f0 / +0x2f4: DC engine configuration [guess: names]. +0x2f0
 * [8:0] = 0xe9 (0xe2 alternative); +0x2f4: [31] 0, [29:28] 1, [24] 1,
 * [20:16] 0x18, [14:12] 2, [8:0] 1 (written in that order; whole value
 * 0x11182001). Observed 0 / 0. [static][measured] */
#define MT7996_PHY_RFINTF_DCE_CFG0(band, chain) MT7996_PHY_RFINTF(band, chain, 0x2f0u)
#define   MT7996_PHY_RFINTF_DCE_CFG0_VAL  HW_GENMASK(8, 0)
#define MT7996_PHY_RFINTF_DCE_CFG1(band, chain) MT7996_PHY_RFINTF(band, chain, 0x2f4u)
#define   MT7996_PHY_RFINTF_DCE_CFG1_TTG_RX 0x11182001u
/* RFINTF +0x30c: [25] = 0, [24] = 0, [17:8] = 0x200, [7:0] = 0 (TTG RX).
 * Observed 0x00020000. [static][measured] */
#define MT7996_PHY_RFINTF_730C(band, chain) MT7996_PHY_RFINTF(band, chain, 0x30cu)
/* RFINTF +0x3bc (chain 0 of the band only): IQ engine status, RO. [29] =
 * done; the firmware waits for it (3 reads, 10 ms) once per chain but
 * always on chain 0's address (~2,900 reads at 20 MHz). Observed
 * 0xc0003003 at start, 0xa0000000 done. In the TX IQ mode (0xc, phy_cal.h
 * T1{8,12,16}) the firmware waits for [30] == 0 instead (3 reads, 100 ms;
 * observed 0xc000xxxx at start, 0xa0000000 done, ~1,950 reads per run at
 * 2.4 GHz). [static][measured] */
#define MT7996_PHY_RFINTF_IQE_STS(band)   MT7996_PHY_RFINTF(band, 0, 0x3bcu)
#define   MT7996_PHY_RFINTF_IQE_STS_DONE  HW_BIT(29)
#define   MT7996_PHY_RFINTF_IQE_STS_BUSY  HW_BIT(30)
/* RFINTF +0x3c0: IQ engine per-bin response, RO: the bin selected by
 * IQE_CFG0 [15:8] while IQE_CFG0 [2] = 1 (read 5 us after setting it). Two
 * 14-bit two's complement Q12 fields: [29:16] (observed -92..+206) and
 * [13:0] (3907..4191, i.e. about 1.0). [static][measured]
 * In the TX IQ mode (0xc, phy_cal.h T1{8,12,16}; IQBIN_ACC [1] = 1 and
 * IQE_CFG0 [1] = 1 around the reads) IQE_CFG0 [15:8] = 0xfa..0xff and
 * 0xc0..0xc6 read the engine's 13 filter taps in [13:0] ([29:16] unused)
 * [inferred: taps -6..6]; bins 1..0x40 and 0x40..0x7f give 16-bit I [15:0]
 * / Q [31:16] responses, the image pairs s, 0x40 - s and 0x80 - s, 0x40 + s.
 * Both are read from RFINTF(band, 0) whatever the WF. [static][measured] */
#define MT7996_PHY_RFINTF_IQBIN(band, chain) MT7996_PHY_RFINTF(band, chain, 0x3c0u)
#define   MT7996_PHY_RFINTF_IQBIN_HI      HW_GENMASK(29, 16)
#define   MT7996_PHY_RFINTF_IQBIN_LO      HW_GENMASK(13, 0)
#define MT7996_PHY_IQBIN_TAP(k)           ((k) < 0 ? 0x100u + (k) : 0xc0u + (k))	/* k -6..6 -> IQE_CFG0 [15:8] */
/* RFINTF +0x3e8: DC engine status, RO. [26:24] = 0 when the chain's DC
 * engine is idle (waited 3 reads, 1 ms). Observed 0x010c231f busy ->
 * 0x000c2221. [static][measured] */
#define MT7996_PHY_RFINTF_DCE_STS(band, chain) MT7996_PHY_RFINTF(band, chain, 0x3e8u)
#define   MT7996_PHY_RFINTF_DCE_STS_BUSY  HW_GENMASK(26, 24)

/*
 * DBG_OVR, RFINTF +0xa24, RW: firmware debug-override word. When a table
 * item's enable bit is set, the WM's PHY table programmer reads it twice and
 * applies the packed (calibrated) data only if [27:16] == 0xbb1 and [0] = 1;
 * otherwise it writes the item's defaults. [static]
 * The same word is word 5 of slot 8, half A, of the RX IQ compensation
 * filter bank (MT7996_PHY_RXIQC_A(band, chain, 8, 5), below) and the class-1
 * group mask of the calibration failure log (MT7996_PHY_CALLOG_*, below):
 * V:e0044c18 reads it twice and returns 0x7fffffff (override; also stored to
 * WM 0xe008117c) when the first read's [27:16] == 0xbb1 and the second's [0]
 * = 1, else 0x79237932; the restores of the filter bank, the per-gain table,
 * the TX gain-step codes and the loop-DC rows (V:e004500a / V:e0044f48 /
 * V:e0044e98 / V:e0044dde) consult it only when WM word 0xe01ec1ac bit 6 / 5
 * / 8 / 28 is set (phy_cal.h MT7996_PHY_DCOC_FAIL_FLAGS). Bits 6, 5 and 8 are
 * not set in the stock load; bit 28 (loop DC) is, so V:e0044dde reads the word
 * twice per WF at the end of every MT7977 loop-DC call and at every channel
 * set (on band 2 it then reads a 320 MHz filter tap, 0x0fb23ff5). The save
 * of the TX LO / IQ codes (phy_cal.h restore flag 0x200, T1{9,13,17}) calls
 * V:e0044c18 on every call, whatever the range check found: without the
 * override a failing class of codes is clamped; its write-back does not
 * consult the word. [static, measured]
 */
#define MT7996_PHY_DBG_OVR(band, chain)   MT7996_PHY_RFINTF(band, chain, 0xa24u)
#define   MT7996_PHY_DBG_OVR_MAGIC        HW_GENMASK(27, 16)
#define   MT7996_PHY_DBG_OVR_MAGIC_VAL    0xbb1u
#define   MT7996_PHY_DBG_OVR_EN           HW_BIT(0)

/*
 * CALLOG, RFINTF +0xa14..+0xa28 (words 1..6 of slot 8, half A, of the RX IQ
 * compensation filter bank, MT7996_PHY_RXIQC_A(band, chain, 8, 1..6)), RW:
 * the WM's calibration failure log [name guess] (V:e00453ee, called by the
 * range check V:e00454ee with chain = WF: base 0x83007a14 + band << 20 + WF
 * << 16). Per failing field:
 *   group mask   class 0 (groups 0..23): +0xa20 = +0xa18 | 1 << b; class 1
 *                (g' 24..34): +0xa24 = +0xa1c | 1 << b; class 2 (g'' 35..40):
 *                +0xa28 = +0xa20 | 1 << b; b = g (g <= 13), g + 2 (14..23),
 *                g - 24 (24..27), g - 12 (28..34), g - 35 (35..40);
 *   code mask    +0xa1c = +0xa14 | 1 << code; code 0 fine DC, 1 coarse DC,
 *                2 loop DC, 3 (flag 0x100), 4 TX IQ (0x20000), 5 TX LO / IQ
 *                (0x200, no flag-word OR), 6
 *                (0x40), 7 (0x20), 10 (0x80; that path also sets +0xa14
 *                [19:16] = 5).
 * Each OR takes its base from the word 8 bytes below the destination
 * (V:e0045408..V:e004545e), so the group masks keep only the last failing
 * group plus whatever the source word held (stock: +0xa20 = 8, 0x80, 0x100,
 * then 2; +0xa24 = 6, 0xc, 5: the code mask's bit 2 carried in). Not in the
 * 430-word D-die backup list: the words persist until a filter-bank restore
 * rewrites slot 8 (only at BW320, 12 slots: T1[22], op 8 on band 2). The
 * class-1 word is DBG_OVR (above): a 160 MHz failure writes its group mask
 * there; the log alone cannot make [27:16] = 0xbb1 (group bits <= 22, code
 * bits <= 10, +0xa14 [19:16] = 5 from the flag-0x80 path), but when slot 8
 * holds a 320 MHz filter the source words are filter taps. Besides the log
 * and V:e0044c18, T1[6], [9], [10], [13], [14], [17] read +0xa24 (stock
 * census; T1[9], [13], [17] through V:e0044c18 in their save, twice per
 * call, phy_cal.h restore flag 0x200). Stock: loop-DC failures (phy_cal.h
 * mask bit 10) on die 1 chain 0
 * only, ending +0xa1c = 4, +0xa20 = 2, +0xa24 = 5; at the band-1 channel set
 * that chain's slot 8 thus reads main tap 0 ([29:16] of +0xa24) where chains
 * 1..3 read the default 0x10000000. Whether the receiver selects slot 8
 * below BW320 is not established. [static, measured]
 */
#define MT7996_PHY_CALLOG_SRC(band, chain)   MT7996_PHY_RFINTF(band, chain, 0xa14u)
#define MT7996_PHY_CALLOG_A18(band, chain)   MT7996_PHY_RFINTF(band, chain, 0xa18u)
#define MT7996_PHY_CALLOG_CODE(band, chain)  MT7996_PHY_RFINTF(band, chain, 0xa1cu)
#define MT7996_PHY_CALLOG_G20(band, chain)   MT7996_PHY_RFINTF(band, chain, 0xa20u)	/* class 0 */
/* class 1 (+0xa24): MT7996_PHY_DBG_OVR */
#define MT7996_PHY_CALLOG_G320(band, chain)  MT7996_PHY_RFINTF(band, chain, 0xa28u)	/* class 2 */
#define   MT7996_PHY_CALLOG_CODE_LOOPDC      HW_BIT(2)
#define   MT7996_PHY_CALLOG_CODE_TXLOIQ      HW_BIT(5)	/* flag 0x200, T1{9,13,17} */

/* RFINTF +0x600..+0xc8c: per-chain table windows programmed by the WM PHY
 * table programmer on channel set / calibration restore; offsets, row counts
 * and defaults in phy_map.h (MT7996_PHY_TBLPROG_LIST). Not named. [static] */
#define MT7996_PHY_RFINTF_TBL_FIRST       0x7600u
#define MT7996_PHY_RFINTF_TBL_LAST        0x7c8cu

/*
 * TX LO-leakage and TX IQ codes per TX gain step [names guess], RFINTF
 * +0x600..+0x69c per chain: the WM table programmer's item 9 window
 * (phy_map.h, default 0), restore flag 0x200 (phy_cal.h
 * MT7996_PHY_CAL_RESTORE_TXLOIQ), written by the engine of T1{9,13,17}
 * (phy_cal.h mask bit 13, MT7996_PHY_TXCAL_*). Step s = 0..15 (MT7976) /
 * 0..11 (MT7977) of 32 slots:
 *   LO I = signed byte (s & 3) of +0x660 + (s & ~3);
 *   LO Q = signed byte (s & 3) of +0x680 + (s & ~3);
 *   IQ A = signed 9-bit [8:0] (s even) / [24:16] (s odd) of +0x600 +
 *          4 (s >> 1);
 *   IQ B = signed 7-bit [6:0] of byte (s & 3) of +0x640 + (s & ~3).
 * Which of A and B is gain and which phase is not established. The body
 * clears the words of steps 0..19 first; the save packs all 40 words and the
 * write-back writes those of the calibrated steps (20 MT7976, 15 MT7977) or
 * 0 to all 40 (phy_cal.h "Restore flags"); phyCalBasicConfig item 16 zeroes
 * all 40 before op 8 at every channel set. Stock: MT7976 LO I -9..2, Q
 * 3..16, A -8..1, B 15..19; MT7977 LO I -13..16, Q -16..18, A -3..33, B
 * -4..5; the channel-set write-backs of another cold load lie within +-2 LSB
 * of these (the spread between cold loads). Read by no WM code but the body,
 * its plausibility check and the save: TX-side [inferred: the TX DC (LO)
 * and IQ compensation per TX gain index; which block applies them is not
 * established]. Not in the D-die backup list. [static][measured]
 */
#define MT7996_PHY_TXLO_I(band, chain, s) MT7996_PHY_RFINTF(band, chain, 0x660u + ((s) & ~3u))	/* byte s & 3 */
#define MT7996_PHY_TXLO_Q(band, chain, s) MT7996_PHY_RFINTF(band, chain, 0x680u + ((s) & ~3u))	/* byte s & 3 */
#define MT7996_PHY_TXLOIQ_A(band, chain, s) MT7996_PHY_RFINTF(band, chain, 0x600u + 4u * ((s) >> 1))
#define MT7996_PHY_TXLOIQ_B(band, chain, s) MT7996_PHY_RFINTF(band, chain, 0x640u + ((s) & ~3u))
#define   MT7996_PHY_TXLO_BYTE(s)         HW_GENMASK(8u * ((s) & 3u) + 7u, 8u * ((s) & 3u))	/* LO I / Q */
#define   MT7996_PHY_TXLOIQ_A_EVEN        HW_GENMASK(8, 0)	/* s even */
#define   MT7996_PHY_TXLOIQ_A_ODD         HW_GENMASK(24, 16)	/* s odd */
#define   MT7996_PHY_TXLOIQ_B_FIELD(s)    HW_GENMASK(8u * ((s) & 3u) + 6u, 8u * ((s) & 3u))
#define MT7996_PHY_TXLOIQ_SLOTS           32u

/*
 * TX gain-step code table [name guess], RFINTF +0x6b0 / +0x6b4 / +0x6b8 per
 * chain: the WM table programmer's item 8 window (phy_map.h, default 0),
 * restore flag 0x100 (phy_cal.h MT7996_PHY_CAL_RESTORE_TXSTEP), results of
 * T1{7,11,15} (phy_cal.h mask bit 11). Field s (s = 0..7) = byte (s & 3) of
 * word s >> 2, bits [4:0] of the byte: 5-bit two's complement, written by
 * the D-die power meter itself after each measurement with RFINTF +0x300
 * [2:1] = 3: the step's level relative to the reference measurement
 * (WF_COMM +0xd824 [17]); field 0 (the reference) is 0. T1{7,11,15} fill
 * fields 0..6 (MT7976) / 0..5 (MT7977) at TX gain indices top..top-6 /
 * top-5, the digital gain raised 0x10 per step; the unit is not established.
 * +0x6b8 [4:0] is the ninth field; T1{7,11,15} set +0x6b8 [0] when a step's
 * code is outside +-8 (and clear that field). T2{4} and T2{5} save the three
 * words before their own TSSI measurements and write them back after every
 * TX step (they would be overwritten). No firmware code reads them outside
 * these calibrations (stock census): a TX-side hardware input [inferred],
 * not part of the receive path. Stock (die 0, group 0): 0x01000100,
 * 0x00010101, 0; dies 1, 2: e.g. 0x001f1f00, 0x00001e1f, 0. Not in the
 * D-die backup list. [static][measured]
 */
#define MT7996_PHY_TXSTEP_CODES(band, chain, n) MT7996_PHY_RFINTF(band, chain, 0x6b0u + 4u * (n))	/* n 0..2 */
#define   MT7996_PHY_TXSTEP_CODE(s)       HW_GENMASK(8u * ((s) & 3u) + 4u, 8u * ((s) & 3u))	/* word s >> 2 */
#define   MT7996_PHY_TXSTEP_FAIL          HW_BIT(0)	/* word 2 */

/*
 * TX IQ compensation filter rows [guess: name], RFINTF +0x6bc..+0x7d8 per
 * chain: the WM table programmer's item 17 window (phy_map.h), restore flag
 * 0x20000 (phy_cal.h MT7996_PHY_CAL_RESTORE_TXIQ), results of T1{8,12,16}
 * (phy_cal.h mask bit 12). Row group j (0..8, 8 words at +0x6bc + 0x20 j) =
 * TX gain step j of the calibration (TX code 0x3e - j) [inferred: TX side;
 * T2{8} (OTFIQ, MT7996_PHY_OTFIQ_CFG0_RFINTF_CLR) resets group 6 - step of
 * its TX gain sweep, phy_cal.h]. Words 0..6: [13:0] tap 2i, [29:16] tap 2i
 * + 1; word 7: [29:16] tap 14; 15 taps of 14-bit two's complement, tap 7
 * (word 3 [29:16]) is the main tap, Q12 (4096 = 1.0). The calibration writes
 * the taps with read-modify-write keeping [31:30] and [15:14] (word 7 keeps
 * [15:0]). Group 0 is stored with 14-bit fields, groups 1..8 with 8-bit side
 * taps (sign-extended to 14 on write-back) and a 14-bit main tap. Defaults
 * (no record, phyCalBasicConfig item 16): 0x10000000 in word 3, all else 0.
 * Stock: MT7976 groups 0..6 and MT7977 groups 0..5 calibrated, the rest
 * default; main tap 4024..4165, side taps -22..+23 (no side tap of groups
 * 1..8 near the 8-bit limit). Read only by T1{8,12,16} (and written by
 * T2{8}). How the hardware picks a row group by TX gain index is not
 * established. [static][measured]
 */
#define MT7996_PHY_TXIQC(band, chain, step, w) \
	MT7996_PHY_RFINTF(band, chain, 0x6bcu + 0x20u * (step) + 4u * (w))	/* step 0..8, w 0..7 */
#define   MT7996_PHY_TXIQC_TAP_LO         HW_GENMASK(13, 0)
#define   MT7996_PHY_TXIQC_TAP_HI         HW_GENMASK(29, 16)
#define   MT7996_PHY_TXIQC_MAIN_WORD      3u	/* [29:16] */
#define   MT7996_PHY_TXIQC_DEFAULT_MAIN   0x10000000u
#define MT7996_PHY_TXIQC_STEPS            9u

/*
 * Per-gain RX IQ table [guess: name], RFINTF +0x820..+0x868 per chain: the
 * WM table programmer's item 5 window (phy_map.h), restore flag 0x20
 * (phy_cal.h MT7996_PHY_CAL_RESTORE_FIQ), written by T1[23]. 15 words in
 * three interleaved arrays, i = 0..4: A_i = +0x820 + 8 i, B_i = +0x824 +
 * 8 i, C_i = +0x858 + 4 i. Entry s (calibration step, 0..17): gain = 10
 * bits at [9:0] (s even) or [25:16] (s odd) of A_(s>>2) (s bit 1 clear) or
 * B_(s>>2) (bit 1 set), Q9 (0x200 = 1.0); phase = byte (s & 3) of
 * C_(s>>2), signed. B_4 has no entries and C_4 two (table 0xe006fee4).
 * Entry 0 (A_0 [9:0], C_0 [7:0]) is also where the engine leaves each
 * measurement. Defaults (no valid record, or not 20 MHz): A, B =
 * 0x02000200, C = 0; a stored table is written back with only its fields
 * (B_4 = 0). Range check (V:e00454ee flag 0x20): gain in [0x180, 0x280],
 * phase within +-0x60. [static][measured]
 */
#define MT7996_PHY_FIQ_A(band, chain, i)  MT7996_PHY_RFINTF(band, chain, 0x820u + 8u * (i))
#define MT7996_PHY_FIQ_B(band, chain, i)  MT7996_PHY_RFINTF(band, chain, 0x824u + 8u * (i))
#define MT7996_PHY_FIQ_C(band, chain, i)  MT7996_PHY_RFINTF(band, chain, 0x858u + 4u * (i))
#define   MT7996_PHY_FIQ_GAIN_LO          HW_GENMASK(9, 0)
#define   MT7996_PHY_FIQ_GAIN_HI          HW_GENMASK(25, 16)
#define   MT7996_PHY_FIQ_GAIN_ONE         0x200u
#define   MT7996_PHY_FIQ_DEFAULT_AB       0x02000200u
#define MT7996_PHY_FIQ_ENTRIES            18u
#define MT7996_PHY_FIQ_GAIN_MIN           0x180u
#define MT7996_PHY_FIQ_GAIN_MAX           0x280u
#define MT7996_PHY_FIQ_PHASE_MAX          0x60	/* signed, +- */

/*
 * RX IQ compensation filter bank [guess: name], RFINTF +0x8d0..+0xc8c per
 * chain: the WM table programmer's item 6 window (phy_map.h), restore flag
 * 0x40 (phy_cal.h MT7996_PHY_CAL_RESTORE_RXIQC), results of T1{18..22}.
 * Slot j (0..11, one per calibration gain step): half A = 10 words at
 * +0x8d0 + 0x28 j, half B = 10 words at +0xab0 + 0x28 j. The IQ engine
 * leaves each measurement in slot 0's addresses (the live slot); the
 * firmware reads A and B there, packs them with slot j's field widths
 * (table 0xe006f944) into its record and writes slot j for every stored
 * step. [static][measured]
 * Word layout of a half (slot 0 widths): +0x00 [31] flag; +0x04 [13:0] tap
 * 0; +0x08..+0x20 [13:0] tap 2n - 1, [29:16] tap 2n (n = 1..7); +0x24
 * none. 15 taps of 14-bit two's complement; half A's tap 8 (+0x14
 * [29:16]) is the main tap, Q12 (4096 = 1.0). Slots 1..11 keep taps 0..4,
 * 9, 11..14 as 8 bits (sign-extended to 14 on write-back) and taps 5..8,
 * 10 as 14 bits. Defaults (no valid record): 0x10000000 at +0x14 of half
 * A, everything else 0.
 * Measured stock values: half A main tap 4084..4128 at 20 MHz, 3944..4167
 * at 40..320 MHz, other A taps -75..+32; half B [guess: the cross (I/Q)
 * filter] 0 at 20 MHz, at 40..320 MHz tap 8 -32..-9 (2.4 GHz) / -13..+115
 * (5/6 GHz) and the other taps within +-9; [31] flag 0. [measured]
 * Range check after each measurement (V:e00454ee flag 0x40): main tap in
 * [0xc00, 0x1400], else +0x8d8 [7:0] and +0x8e0 [7:0] within +-0x100; a
 * failure sets WM word 0xe01ec1ac |= 0x40 and logs. [static]
 * Fit output: in V:e004a11e the engine leaves each chain's fitted pair in
 * chain 1's live slot address (RFINTF(band, 1, +0x8d0..+0x8f4 /
 * +0xab0..+0xad4)) whichever chain was loaded; the firmware copies it out
 * after every chain and writes each chain's own live slot at the end
 * (V:e004a80a / V:e004a818). [measured]
 * Overlap: slot 8 half A words 1..6 (+0xa14..+0xa28) are also the
 * calibration failure log (MT7996_PHY_CALLOG_*, above), which band 1 never
 * rewrites (it restores slots 0..5 only); word 5 (+0xa24) is also DBG_OVR.
 * [static, measured: stock census]
 */
#define MT7996_PHY_RXIQC_A(band, chain, slot, w) \
	MT7996_PHY_RFINTF(band, chain, 0x8d0u + 0x28u * (slot) + 4u * (w))	/* w 0..9 */
#define MT7996_PHY_RXIQC_B(band, chain, slot, w) \
	MT7996_PHY_RFINTF(band, chain, 0xab0u + 0x28u * (slot) + 4u * (w))
#define   MT7996_PHY_RXIQC_W_FLAG         HW_BIT(31)	/* word 0 */
#define   MT7996_PHY_RXIQC_W_TAP_LO       HW_GENMASK(13, 0)
#define   MT7996_PHY_RXIQC_W_TAP_HI       HW_GENMASK(29, 16)
#define   MT7996_PHY_RXIQC_MAIN_WORD      5u	/* half A word 5 [29:16] */
#define   MT7996_PHY_RXIQC_MAIN_ONE       0x1000u
#define   MT7996_PHY_RXIQC_DEFAULT_MAIN   0x10000000u	/* half A word 5 without a record */
#define MT7996_PHY_RXIQC_SLOTS            12u
#define MT7996_PHY_RXIQC_MAIN_MIN         0x0c00u
#define MT7996_PHY_RXIQC_MAIN_MAX         0x1400u
#define MT7996_PHY_RXIQC_FIT_CHAIN        1u	/* V:e004a11e result window */

/* ========================================================================
 * Other per-chain words (sub-blocks 0x2000, 0x8000..0xb000, 0xd000, 0xe000,
 * 0xf000). TSSI (0xc000), the DPD LUTs and their gain and path words
 * (+0xa000..+0xa9ff, +0xaf00..+0xaf44), the TX flatness FIR
 * (+0xd030..+0xd044), +0xd3a0, the OTFIQ result slots (+0xd600..+0xd77c)
 * and the TX power fields the calibration init saves (+0xd104..+0xd2ac,
 * MT7996_PHY_TXPWR_SAVE_*) are in phy_cal.h; 0x8300e81c (DFE engine pulse)
 * is in dfe.h.
 * ======================================================================== */

/* Per-chain +0x2000..+0x2080 (no map entry): the per-band PHY init's
 * per-chain register table writes 9 of its words (+0x00, +0x04, +0x34,
 * +0x38, +0x40, +0x70, +0x74 = 0; +0x10 = 0x0040a0a0, +0x24 = 0x0020d0d0;
 * phy_map.h MT7996_PHY_REGTBL_*); the channel-set PHY tables A and B
 * (phy_chan.h) write others, and table A's explicit fields +0x0c / +0x18 /
 * +0x28 [23:0] (MT7996_PHY_CHAIN_2000_LO24). The PHY-init routine's tail (V:e012ee16)
 * then clears [23:0] and keeps [31:24] of 32 words +0x2000..+0x2080
 * (without +0x58, list V:e015b4b0) on chains 0..3 of band b when EEPROM
 * 0x196 + b [6] is set (MT7996_PHY_EE_196); on this unit band 2 only
 * (0x83202000..), whose words read 0 before except the table's +0x10 and
 * +0x24, which the clear leaves 0. Role unknown. [static][measured] */
#define MT7996_PHY_CHAIN_2000(band, chain, off) MT7996_PHY_CHAIN_REG(band, chain, 0x2000u + (off))	/* off 0..0x80 */
#define   MT7996_PHY_CHAIN_2000_KEEP      HW_GENMASK(31, 24)
#define   MT7996_PHY_CHAIN_2000_LO24      HW_GENMASK(23, 0)
/* EEPROM image bytes 0..1: the chip ID, 0x7990 (MT7996_PCI_DEVICE_ID, host.h), as mt76 checks
 * it. Our bring-up refuses an image without it: a blank image would otherwise read as every band
 * absent and the per-band init would be skipped without an error. [measured] */
#define MT7996_PHY_EE_CHIP_ID             0x000u	/* u16, little endian */

/* Per-band EEPROM byte 0x196 + band (host EEPROM image): [0] ELNA and [1]
 * EPA (the calibration and channel-set P blocks' P[7] / P[6], phy_cal.h
 * "Calibration init"), [3] with EPA the channel set's P[0x1c]
 * (V:e00d4bfc..V:e00d4c82), [6] clears the band's per-chain +0x2000 block at
 * PHY init (above). Stock band 2 (0x198) = 0x4b. [static][measured] */
#define MT7996_PHY_EE_196(band)           (0x196u + (band))
#define   MT7996_PHY_EE_196_ELNA          HW_BIT(0)
#define   MT7996_PHY_EE_196_EPA           HW_BIT(1)
#define   MT7996_PHY_EE_196_P1C           HW_BIT(3)	/* with EPA */
#define   MT7996_PHY_EE_196_CHAIN_2000_CLR HW_BIT(6)
/* EEPROM 0x1a4 [7]: the WM's calibration init copies it to WM byte 0xe01ec154
 * (V:e00d6de8 -> V:e00e15a0, EEPROM image byte 0x1a4 >> 7); V:e00de40a reads that byte
 * (rfchan.h RFCHAN_RXSET_36A_*). This unit: 0x04 ([7] = 0). EEPROM 0x1a4 [6] is the PHY table
 * B field of MT7996_PHY_WFCOMM_C104. [static] */
#define MT7996_PHY_EE_1A4                 0x1a4u
#define   MT7996_PHY_EE_1A4_B7            HW_BIT(7)
#define   MT7996_PHY_EE_1A4_B6            HW_BIT(6)	/* PHY table B's WF_COMM +0xc104 field */

/* WIFI_CONF 0x190..0x194: 3-bit fields f, at byte 0x190 + f / 2, bits
 * [3 (f % 2) + 2 : 3 (f % 2)] (mt76 MT_EE_WIFI_CONF*): f = band: the band
 * select (1 2.4 GHz, 2 5 GHz, 3 6 GHz; 0 = band absent, the calibration
 * init skips it; V:e00e02be); f = 3 + band: TX path count; f = 6 + band: RX
 * path count (V:e00dffa2). Stock 1, 2, 3; TX 4, 4, 4; RX 4, 4, 5.
 * [static][measured] */
#define MT7996_PHY_EE_CONF_OFF(f)         (0x190u + (f) / 2u)
#define MT7996_PHY_EE_CONF_FIELD(f)       HW_GENMASK(3u * ((f) % 2u) + 2u, 3u * ((f) % 2u))
#define MT7996_PHY_EE_CONF_BAND_SEL(band) (band)
#define   MT7996_PHY_EE_BAND_SEL_2G4      1u
#define   MT7996_PHY_EE_BAND_SEL_LAST     7u
#define MT7996_PHY_EE_CONF_TX_PATH(band)  (3u + (band))
#define MT7996_PHY_EE_CONF_RX_PATH(band)  (6u + (band))

/* 0x199 + band: [2] the FEM type of the PHY-init tail's FEM words
 * (V:e00e111c; adie_init.h), [3] chain 0xe90 [30] = ![3] (V:e00e3858; set:
 * V:e00e2314 takes a path with more calls, not decoded), [5] / [6] DPD
 * (phy_cal.h MT7996_PHY_DPD_SCRATCH_SWEEP). Stock 0x35, 0x22, 0x22: the FEM
 * type on band 0 only, [3] clear on every band. [static][measured] */
#define MT7996_PHY_EE_199(band)           (0x199u + (band))
#define   MT7996_PHY_EE_199_FEM           HW_BIT(2)
#define   MT7996_PHY_EE_199_E90           HW_BIT(3)

/* 0x19c + band: FEM pin options of the pin mux (V:e00ce6a0 via V:e00e105c /
 * V:e00e12ec): [0] polarity of the TX-path pins, [1] of the RX-path pins,
 * [7] of RX-path pins of WF 4, [2] of the other pins, [4] keeps the pins
 * of signals above 0x17 on (unless unused) and sets ADIE_TOP_FEM_49D_SET
 * (adie_init.h). Names [guess]. [static] */
#define MT7996_PHY_EE_19C(band)           (0x19cu + (band))
#define   MT7996_PHY_EE_19C_POL_TX        HW_BIT(0)
#define   MT7996_PHY_EE_19C_POL_RX        HW_BIT(1)
#define   MT7996_PHY_EE_19C_POL_OTHER     HW_BIT(2)
#define   MT7996_PHY_EE_19C_SHARED        HW_BIT(4)
#define   MT7996_PHY_EE_19C_POL_RX_WF4    HW_BIT(7)

/* +0x87ac: [31] = 1, [30] = 0 (D-die chain routing V:e0049472: the TTG RX
 * calibrations on every chain, T2{6} on the MT7977); the per-band PHY init
 * already sets [31:30] = 2 on chains 0..3 (V:e00de924..V:e00de978);
 * +0x87b0: [31] = 0, [30] = 0 (TTG RX). Observed 0x80000000 / 0, before and
 * after the init. No names in any corpus. [static][measured] */
#define MT7996_PHY_CHAIN_87AC(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0x87acu)
#define   MT7996_PHY_CHAIN_87AC_ROUTE     HW_GENMASK(31, 30)
#define   MT7996_PHY_CHAIN_87AC_ROUTE_INIT 2u
#define MT7996_PHY_CHAIN_87B0(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0x87b0u)
/* Per-chain table memories written only by the per-band PHY init (chains
 * 0..3, whole words, V:e00df096..V:e00df13e): +0x9000..+0x92fc = 0x1001,
 * +0x9500..+0x97fc, +0x9a00..+0x9cfc, +0xb000..+0xb2fc (192 words each) and
 * +0xb500..+0xb87c (224 words) = 0. No other WM code addresses these pages
 * and no other stock census site touches them, so the hardware consumes
 * them. Neighbours of the DPD LUT RAM (+0xa000, phy_cal.h). Names and role
 * unknown [guess]. [static] */
#define MT7996_PHY_CHAIN_9000(band, chain, i) MT7996_PHY_CHAIN_REG(band, chain, 0x9000u + 4u * (i))	/* i 0..191 */
#define   MT7996_PHY_CHAIN_9000_INIT      0x00001001u
#define MT7996_PHY_CHAIN_9500(band, chain, i) MT7996_PHY_CHAIN_REG(band, chain, 0x9500u + 4u * (i))	/* i 0..191, = 0 */
#define MT7996_PHY_CHAIN_9A00(band, chain, i) MT7996_PHY_CHAIN_REG(band, chain, 0x9a00u + 4u * (i))	/* i 0..191, = 0 */
#define MT7996_PHY_CHAIN_B000(band, chain, i) MT7996_PHY_CHAIN_REG(band, chain, 0xb000u + 4u * (i))	/* i 0..191, = 0 */
#define MT7996_PHY_CHAIN_B500(band, chain, i) MT7996_PHY_CHAIN_REG(band, chain, 0xb500u + 4u * (i))	/* i 0..223, = 0 */
#define MT7996_PHY_CHAIN_9000_N           192u	/* also 0x9500, 0x9a00, 0xb000 */
#define MT7996_PHY_CHAIN_B500_N           224u
/* +0xd008: [6] set on chains 0..3 by the WM PHY register helper; [7] set on
 * the selected chain (cleared on all for mode 16); DPD (phy_cal.h T2{0..3})
 * clears [12] (restored). The TX LO / IQ calibration (phy_cal.h
 * T1{9,13,17}) writes [7:6] = 1, then 3 on the WF before every LO engine
 * run, around every IQ-phase TX gain setting and in every IQ-phase meter
 * reading (observed 0x00011c50 / 0x00011cd0; restored). Not decoded.
 * [static][measured] */
#define MT7996_PHY_CHAIN_D008(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd008u)
#define   MT7996_PHY_CHAIN_D008_B6        HW_BIT(6)
#define   MT7996_PHY_CHAIN_D008_B7        HW_BIT(7)
/* +0xd010: [7] set by the channel-set D-die RX calibration (phy_cal.h; one
 * read-modify-write per chain, chains 0..3 and chain 4 on bands 1 and 2:
 * stock 0x00002090 unchanged, band 1's chain 4 0 -> 0x80); the routine's
 * other path (V:e00dddec) clears [7] instead and is not taken in the stock
 * channel sets. DPD sets [27] and clears [21] (restored); the TX IQ
 * calibration (phy_cal.h T1{8,12,16}) clears [24] (restored).
 * [static][measured] */
#define MT7996_PHY_CHAIN_D010(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd010u)
#define   MT7996_PHY_CHAIN_D010_B7        HW_BIT(7)
#define   MT7996_PHY_CHAIN_D010_B24       HW_BIT(24)
/* +0xd0ac [name guess]: read-modified on chains 0..4 by every channel set
 * (V:e00d196a, phy_cal.h "Channel set") to a bandwidth-dependent value:
 * 0x00010c5f (BW20), 0x0003e6a8 (BW160), 0x0006b590 (BW320); 0x00010c40
 * before (chain 4: 0). Role unknown; in the receive-only subset.
 * [static][measured] */
#define MT7996_PHY_CHAIN_D0AC(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd0acu)
#define   MT7996_PHY_CHAIN_D0AC_VAL       HW_GENMASK(20, 0)	/* round(0xa000000 / f_MHz) << s, the channel set */
/* +0xd0d0: [0] = 1 on chains 0..3 when MT7996_CBTOP_HW_REV != 0x8a00
 * (V:e00df3a2; E1 parts skip it); reads 0 before. +0xd37c: [4:0] = 0
 * (chains 0..3; observed 3 -> 0), read-modified again by the band-1 / band-2
 * channel sets (V:e00d3c90..). +0xd3b8 [19:0] = 0x01201, +0xd3bc [19:0] =
 * 0x01212 (chains 0..3; observed equal before). All by the per-band PHY init
 * (below). [static][measured] */
#define MT7996_PHY_CHAIN_D0D0(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd0d0u)
#define   MT7996_PHY_CHAIN_D0D0_B0        HW_BIT(0)
#define MT7996_PHY_CHAIN_D37C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd37cu)
#define   MT7996_PHY_CHAIN_D37C_LO        HW_GENMASK(4, 0)
#define MT7996_PHY_CHAIN_D3B8(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd3b8u)
#define MT7996_PHY_CHAIN_D3BC(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd3bcu)
#define   MT7996_PHY_CHAIN_D3BX_LO        HW_GENMASK(19, 0)
#define   MT7996_PHY_CHAIN_D3B8_INIT      0x01201u
#define   MT7996_PHY_CHAIN_D3BC_INIT      0x01212u
/* +0xe01c: [7] set by the channel-set D-die RX calibration (chains 0..4:
 * 0x0200401f -> 0x0200409f, chain 4 of bands 0 and 1 0 -> 0x80); [1:0]
 * retuned by SW-LNA profiles. +0xe024 [0] set (0x42520000 -> 0x42520001;
 * chain 4 of bands 0 and 1 0 -> 1), +0xe034 [29] pulsed while BN_CLK0 / 1
 * [17] are set, by the same calibration (phy_cal.h, its order). +0xe02c
 * [15:8] SW-LNA. The per-band PHY init's register table
 * writes +0xe01c = 0x0200401f and +0xe02c = 0x2505c812 on chains 0..4.
 * [static][measured] */
#define MT7996_PHY_CHAIN_E01C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xe01cu)
#define MT7996_PHY_CHAIN_E024(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xe024u)
#define MT7996_PHY_CHAIN_E02C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xe02cu)
#define MT7996_PHY_CHAIN_E034(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xe034u)
#define   MT7996_PHY_CHAIN_E01C_B7        HW_BIT(7)
#define   MT7996_PHY_CHAIN_E034_B29       HW_BIT(29)
#define   MT7996_PHY_CHAIN_E024_B0        HW_BIT(0)
/* +0xe030: [22] set by the TTG RX calibrations' chain routing (V:e0049472;
 * 0xa80a7122 -> 0xa84a7122). [static][measured] */
#define MT7996_PHY_CHAIN_E030(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xe030u)
#define   MT7996_PHY_CHAIN_E030_B22       HW_BIT(22)
/*
 * +0xe108: calibration word. A WM leaf copies four little-endian words to
 * chains 0..3 of the current band, plus chain 4 on band 2 only
 * (0x8324e108). The disabled D-die AFE/RXFE calibration pulses [31:29].
 * [static]
 */
#define MT7996_PHY_CHAIN_E108(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xe108u)
#define   MT7996_PHY_CHAIN_E108_PULSE     HW_GENMASK(31, 29)
/* +0xf028: whole word 0x7c00 written to chains 0..4 by WM PHY preparation in
 * modes 4, 5, 12. [static] */
#define MT7996_PHY_CHAIN_F028(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xf028u)
/*
 * Synthesizer bookkeeping, chain-0 block of each band: the WM's SX tune
 * stores the SX0 LO frequency in kHz at +0xfc00 and its P[1] at +0xfc04 (SX1:
 * +0xfc08 / +0xfc0c), with the A-die notes ADIE_FREQ_NOTE_*, after the preset
 * tables and before the synthesizer words (not after tuning; sxchan.h). Used
 * as storage, not a control; 0x8301fc00 (band
 * 0, chain-1 block) is read by every channel set: 0x3333 reruns the
 * calibration init, 0xffff selects an extra calibration op (phy_cal.h
 * MT7996_PHY_CHSET_TRIG_*, HAZARD).
 * DPD reads 0x8301fc00..0x8301fc20 + band << 20 as WM scratch words
 * (phy_cal.h MT7996_PHY_DPD_SCRATCH_*); the calibration init and the group
 * loops write the scratch words 0x8301fc10 and 0x8303fc10 / 0x8303fc14
 * (phy_cal.h MT7996_PHY_FW_1FC10, MT7996_PHY_FW_GAINSTEP_*). [static]
 */
#define MT7996_PHY_SX0_FREQ_KHZ(band)     MT7996_PHY_CHAIN_REG(band, 0, 0xfc00u)
#define MT7996_PHY_SX0_PARAM(band)        MT7996_PHY_CHAIN_REG(band, 0, 0xfc04u)
#define MT7996_PHY_SX1_FREQ_KHZ(band)     MT7996_PHY_CHAIN_REG(band, 0, 0xfc08u)
#define MT7996_PHY_SX1_PARAM(band)        MT7996_PHY_CHAIN_REG(band, 0, 0xfc0cu)

/* Offset names, role unknown unless a header says more: rows of the per-band PHY init's
 * register tables (phy_init.h MT7996_PHY_INIT_REGTBL_*; "init", written whole on every
 * band, never read by the init) and of the channel-set PHY tables A / B (phy_chan.h
 * MT7996_CHSET_TBL_A_ROWS / _B_ROWS; "table A" / "table B"), and words the channel set
 * writes by field. [static][measured]
 * +0xd000: the per-chain words around the TX power fields of phy_cal.h
 * (+0xd104..+0xd2ac); +0xe000: calibration words, SW-LNA. */
#define MT7996_PHY_CHAIN_C04C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xc04cu)	/* table A */
#define MT7996_PHY_CHAIN_D0D8(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd0d8u)	/* init */
#define MT7996_PHY_CHAIN_D100(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd100u)	/* table A */
#define MT7996_PHY_CHAIN_D104(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd104u)	/* init */
#define MT7996_PHY_CHAIN_D108(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd108u)	/* init */
#define MT7996_PHY_CHAIN_D10C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd10cu)	/* init */
#define MT7996_PHY_CHAIN_D110(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd110u)	/* init */
#define MT7996_PHY_CHAIN_D114(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd114u)	/* table A */
#define MT7996_PHY_CHAIN_D118(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd118u)	/* init */
#define MT7996_PHY_CHAIN_D11C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd11cu)	/* init */
#define MT7996_PHY_CHAIN_D120(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd120u)	/* init */
#define MT7996_PHY_CHAIN_D124(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd124u)	/* init */
#define MT7996_PHY_CHAIN_D204(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd204u)	/* init */
#define MT7996_PHY_CHAIN_D250(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd250u)	/* table A */
#define MT7996_PHY_CHAIN_D254(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd254u)	/* table A */
#define MT7996_PHY_CHAIN_D258(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd258u)	/* init */
#define MT7996_PHY_CHAIN_D25C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd25cu)	/* init */
#define MT7996_PHY_CHAIN_D260(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd260u)	/* table A */
#define MT7996_PHY_CHAIN_D264(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd264u)	/* table A */
#define MT7996_PHY_CHAIN_D268(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd268u)	/* init */
#define MT7996_PHY_CHAIN_D26C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd26cu)	/* table A */
#define MT7996_PHY_CHAIN_D270(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd270u)	/* init */
#define MT7996_PHY_CHAIN_D274(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd274u)	/* table A */
#define MT7996_PHY_CHAIN_D278(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd278u)	/* init */
#define MT7996_PHY_CHAIN_D27C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd27cu)	/* table A */
#define MT7996_PHY_CHAIN_D280(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd280u)	/* table A */
#define MT7996_PHY_CHAIN_D284(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd284u)	/* init */
#define MT7996_PHY_CHAIN_D288(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd288u)	/* init */
#define MT7996_PHY_CHAIN_D28C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd28cu)	/* table A */
#define MT7996_PHY_CHAIN_D290(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd290u)	/* init */
#define MT7996_PHY_CHAIN_D294(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd294u)	/* init */
#define MT7996_PHY_CHAIN_D298(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd298u)	/* init */
#define MT7996_PHY_CHAIN_D29C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd29cu)	/* init */
#define MT7996_PHY_CHAIN_D2A0(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd2a0u)	/* table A */
#define MT7996_PHY_CHAIN_D2A4(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd2a4u)	/* init */
#define MT7996_PHY_CHAIN_D2A8(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd2a8u)	/* table A */
#define MT7996_PHY_CHAIN_D2AC(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd2acu)	/* init */
#define MT7996_PHY_CHAIN_D2C0(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd2c0u)	/* table A */
#define MT7996_PHY_CHAIN_D2CC(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd2ccu)	/* table A */
#define MT7996_PHY_CHAIN_D2D0(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd2d0u)	/* table A */
#define MT7996_PHY_CHAIN_D2D4(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd2d4u)	/* table A */
#define MT7996_PHY_CHAIN_D330(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd330u)	/* init */
#define MT7996_PHY_CHAIN_D334(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd334u)	/* init */
#define MT7996_PHY_CHAIN_D338(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd338u)	/* init */
#define MT7996_PHY_CHAIN_D33C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd33cu)	/* init */
#define MT7996_PHY_CHAIN_D3AC(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xd3acu)	/* init */
#define MT7996_PHY_CHAIN_E048(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xe048u)	/* init */
#define MT7996_PHY_CHAIN_E070(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xe070u)	/* init */
#define MT7996_PHY_CHAIN_E07C(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xe07cu)	/* table B */
#define MT7996_PHY_CHAIN_E140(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xe140u)	/* init */
#define MT7996_PHY_CHAIN_E144(band, chain) MT7996_PHY_CHAIN_REG(band, chain, 0xe144u)	/* init */

/* ========================================================================
 * Band control block PHYRX_CTRL_BN: 0x83080000 + band<<20 + 0x0000..0xdfff
 * (ROM modules 0x61/0x63/0x65/0x67/0x6b/0x6f, WFRDM_PHYRX 0x33/0x3a).
 * Datapath order from RST_CTRL: RFINTF -> RXFE -> RXTD -> FFT -> OPOST,
 * with PHYMISC/PHYMUX/PHYCTRL and side blocks TOAE, RBIST, DPD_CAL, RDD, TSSI.
 * ======================================================================== */

#define MT7996_PHY_BN(band, off)          MT7996_PHY_BAND_REG(band, 0x80000u + (off))

/* +0x0000: [12] cleared by WM PHY preparation (also at 0x83090000). [static] */
#define MT7996_PHY_BN_CTRL0(band)         MT7996_PHY_BN(band, 0x0000u)
#define   MT7996_PHY_BN_CTRL0_B12         HW_BIT(12)
/* [28:24] and [20:16]: = 1 at a detector start
 * (V:e00cad1a: band 0's word for detector 2, the band's own for detectors 0 / 1). Role not
 * decoded [inferred: detector enables]. Stock 0 -> 0x01000000 -> 0x01010000. [static][measured] */
#define   MT7996_PHY_BN_CTRL0_F28_24      HW_GENMASK(28, 24)
#define   MT7996_PHY_BN_CTRL0_F20_16      HW_GENMASK(20, 16)

/*
 * BN_CLK0 +0x0004 / BN_CLK1 +0x0008, RW: band clock-enable words (no ROM
 * name). Value 0 in normal RX (decoder running) [measured]; the WM PHY init
 * writes 0 to both on all three bands; the WM PHY register helper writes
 * whole word 0x01b60000 to both (and to 0x83090004/08). [static]
 * ROM halToaeEnablePhyClk keeps [31:24], replaces [23:0] with en ? 0x400 : 0
 * (enable compared == 1; its address uses band << 16, not << 20). [static]
 * After a firmware capture arm both read 0x02000000. [measured]
 * Bits not listed: not decoded; [inferred] per-block clock enables in
 * RST_CTRL bit order.
 */
#define MT7996_PHY_BN_CLK0(band)          MT7996_PHY_BN(band, 0x0004u)
#define MT7996_PHY_BN_CLK1(band)          MT7996_PHY_BN(band, 0x0008u)
/* TOAE clock (ROM name of the setter). Setting it does not make ICAP
 * source 6 strobe. [measured] */
#define   MT7996_PHY_BN_CLK_TOAE          HW_BIT(10)
/* Pulsed by the channel-set D-die RX calibration: set on CLK0 then CLK1
 * around the per-chain CHAIN_E034 [29] pulse, cleared on CLK1 then CLK0
 * (0 -> 0x00020000 -> 0 on every band). [static][measured] */
#define   MT7996_PHY_BN_CLK_B17           HW_BIT(17)
/* Set by the firmware's capture start on both words. [static, measured] */
#define   MT7996_PHY_BN_CLK_CAPTURE       HW_BIT(25)
/* CSI / AHB-bridge clock: required for the AHB bridge to write (set it here
 * and in WF_COMM 0x3050/0x3054). Not an ICAP enable. [measured] */
#define   MT7996_PHY_BN_CLK_CSI           HW_BIT(29)
#define   MT7996_PHY_BN_CLK_KEEP          HW_GENMASK(31, 24)	/* preserved by the ROM helper */
/* BN_CLK0 / BN_CLK1 [8]: set by the detector start (V:e0133a02 with its a3 = 0: both words read,
 * then both written). Not decoded. [static][measured] */
#define   MT7996_PHY_BN_CLK_B8            HW_BIT(8)

/*
 * RST_CTRL, +0x0014, RW, reset 0, 0 in normal RX. [measured]
 * [30:28] CSR resets, [24] and [17:0] logic resets (1 = held in reset).
 * WM PHY preparation sets [17:0] then writes 0 (also at 0x83090014); RX-gain
 * calibration pulses [2] RXTD; every calibration end writes 0x0103ffff
 * ([24], [17:0]) then 0 (MT7996_PHY_RST_CALEND below). [static: ROM names]
 */
#define MT7996_PHY_RST_CTRL(band)         MT7996_PHY_BN(band, 0x0014u)
#define   MT7996_PHY_RST_RXFE_CSR         HW_BIT(30)
#define   MT7996_PHY_RST_RFINTF_CSR       HW_BIT(29)
#define   MT7996_PHY_RST_TXFD_CSR         HW_BIT(28)
#define   MT7996_PHY_RST_RSVD0            HW_BIT(24)
#define   MT7996_PHY_RST_TXFD             HW_BIT(17)
#define   MT7996_PHY_RST_TXTD             HW_BIT(16)
#define   MT7996_PHY_RST_TXFE             HW_BIT(15)
#define   MT7996_PHY_RST_TSSI             HW_BIT(14)
#define   MT7996_PHY_RST_RDD              HW_BIT(13)
#define   MT7996_PHY_RST_MAC2PHY          HW_BIT(12)
#define   MT7996_PHY_RST_DPD_CAL          HW_BIT(11)
#define   MT7996_PHY_RST_RBIST            HW_BIT(10)	/* the band's own RBIST (ICAP-style) block */
#define   MT7996_PHY_RST_TOAE_CAL         HW_BIT(9)
#define   MT7996_PHY_RST_TOAE             HW_BIT(8)
#define   MT7996_PHY_RST_PHYCTRL          HW_BIT(7)
#define   MT7996_PHY_RST_PHYMUX           HW_BIT(6)
#define   MT7996_PHY_RST_PHYMISC          HW_BIT(5)
#define   MT7996_PHY_RST_OPOST            HW_BIT(4)
#define   MT7996_PHY_RST_FFT              HW_BIT(3)
#define   MT7996_PHY_RST_RXTD             HW_BIT(2)
#define   MT7996_PHY_RST_RXFE             HW_BIT(1)
#define   MT7996_PHY_RST_RFINTF           HW_BIT(0)
#define   MT7996_PHY_RST_LOGIC_ALL        HW_GENMASK(17, 0)

/* PKT_RST, +0x0018, RW: [7:0] per-packet reset disables (ROM
 * halToaeDisablePerPktRst writes the byte). Reads 0x200 in normal RX ([9]
 * set, outside that byte, undecoded). [static, value measured] */
#define MT7996_PHY_PKT_RST(band)          MT7996_PHY_BN(band, 0x0018u)
#define   MT7996_PHY_PKT_RST_DIS          HW_GENMASK(7, 0)

/*
 * MISC_CTRL +0x0038 (IRQ enable), MISC_CTRL_1 +0x003c (IRQ clear, same bit
 * layout), MISC_IRQ_STA +0x0040 (RO status). 0 in normal RX. [static: ROM
 * names; value measured] The RDD2 start sets [11:10] in MISC_CTRL.
 */
#define MT7996_PHY_MISC_CTRL(band)        MT7996_PHY_BN(band, 0x0038u)
#define MT7996_PHY_MISC_CLR(band)         MT7996_PHY_BN(band, 0x003cu)
#define MT7996_PHY_MISC_IRQ_STA(band)     MT7996_PHY_BN(band, 0x0040u)
#define   MT7996_PHY_MISC_BF_LQ_IND_VALID HW_BIT(17)
#define   MT7996_PHY_MISC_AHBBRG          HW_BIT(16)	/* AHB bridge IRQ */
#define   MT7996_PHY_MISC_RDD3_ERROR      HW_BIT(15)
#define   MT7996_PHY_MISC_RDD3_WAKEUP     HW_BIT(14)
#define   MT7996_PHY_MISC_RDD2_ERROR      HW_BIT(13)
#define   MT7996_PHY_MISC_RDD2_WAKEUP     HW_BIT(12)
#define   MT7996_PHY_MISC_RDD1_ERROR      HW_BIT(11)
#define   MT7996_PHY_MISC_RDD1_WAKEUP     HW_BIT(10)
#define   MT7996_PHY_MISC_PFMU_IBF_RENEW  HW_BIT(9)
#define   MT7996_PHY_MISC_PFMU_IBF_EXPIRE HW_BIT(8)
#define   MT7996_PHY_MISC_SPI_FAIL_IND    HW_BIT(7)
#define   MT7996_PHY_MISC_TRX_WDT_TRIG    HW_BIT(6)
#define   MT7996_PHY_MISC_MUTX_LQ_ERR     HW_BIT(5)
#define   MT7996_PHY_MISC_MUTX_TX_ERR     HW_BIT(4)
#define   MT7996_PHY_MISC_RDD_ERROR       HW_BIT(3)
#define   MT7996_PHY_MISC_RDD_WAKEUP      HW_BIT(2)
#define   MT7996_PHY_MISC_WIFI_ERROR_TRIG HW_BIT(1)
#define   MT7996_PHY_MISC_PHYCTRL_INT     HW_BIT(0)

/* +0x0044: the per-band PHY init sets [31] = 1, [10:8] = 0, [26] = 0
 * (observed 0x00fff8ff -> 0x80fff8ff). Not decoded. [static][measured] */
#define MT7996_PHY_BN_0044(band)          MT7996_PHY_BN(band, 0x0044u)
#define   MT7996_PHY_BN_0044_B31          HW_BIT(31)
#define   MT7996_PHY_BN_0044_B26          HW_BIT(26)
#define   MT7996_PHY_BN_0044_F10_8        HW_GENMASK(10, 8)

/* +0x0050: = 0xc100 by the WM PHY register helper in modes 5, 12 (the TTG
 * RX calibrations). Not decoded. [static][measured] */
#define MT7996_PHY_BN_0050(band)          MT7996_PHY_BN(band, 0x0050u)

/* +0x0070: background-receiver control. The RDD2 start writes 0x10001001,
 * then [6:4] = (monitor setting) - 1; the WM PHY register helper sets
 * [31:24] and [15:8]. The TX IQ calibration (phy_cal.h T1{8,12,16}) then
 * writes [11:8] = the one-hot loopback receiver chain. Not decoded.
 * [static][measured] */
#define MT7996_PHY_BN_BGRX(band)          MT7996_PHY_BN(band, 0x0070u)
#define   MT7996_PHY_BN_BGRX_RDD2_INIT    0x10001001u
#define   MT7996_PHY_BN_BGRX_6_4          HW_GENMASK(6, 4)
#define   MT7996_PHY_BN_BGRX_RXSEL        HW_GENMASK(11, 8)

/*
 * CSI SRAM access (PHYRX_CTRL_BN_CSI). [static: ROM names]
 * CSI_MAN_CHANNEL_SEL_0..3 +0x0c04..+0x0c10: sixteen 6-bit manual CSI
 * indexes, four per word at [5:0] [13:8] [21:16] [29:24] (SEL_0 holds
 * indexes 12..15, SEL_3 indexes 0..3); SEL_0 [31] = manual-index enable.
 * CSI_MEM_CTRL +0x0c14: [24] SRAM CS, [12:0] SRAM address.
 * CSI_MEM_DATA_IN +0x0c18 [27:0], CSI_MEM_DATA_OUT +0x0c1c [27:0] (RO).
 */
#define MT7996_PHY_CSI_MAN_SEL(band, n)   MT7996_PHY_BN(band, 0x0c04u + 4u * (n))
#define   MT7996_PHY_CSI_MAN_SEL_EN       HW_BIT(31)
#define   MT7996_PHY_CSI_MAN_IDX(k)       HW_GENMASK(8 * (k) + 5, 8 * (k))
#define MT7996_PHY_CSI_MEM_CTRL(band)     MT7996_PHY_BN(band, 0x0c14u)
#define   MT7996_PHY_CSI_MEM_CS           HW_BIT(24)
#define   MT7996_PHY_CSI_MEM_ADDR         HW_GENMASK(12, 0)
#define MT7996_PHY_CSI_MEM_DI(band)       MT7996_PHY_BN(band, 0x0c18u)
#define MT7996_PHY_CSI_MEM_DO(band)       MT7996_PHY_BN(band, 0x0c1cu)
#define   MT7996_PHY_CSI_MEM_DATA         HW_GENMASK(27, 0)

/*
 * TOAE (time-of-arrival estimation) band registers, +0x0c58..+0x1000.
 * [static: ROM names] The TOAE path works on frequency-domain channel
 * estimates (H peak, tau, FFT input), not ADC samples.
 */
#define MT7996_PHY_TOAE_RSVD0(band)       MT7996_PHY_BN(band, 0x0c58u)
#define MT7996_PHY_TOAE_RSVD1(band)       MT7996_PHY_BN(band, 0x0c5cu)
/* TOAE_CAL_0..3 (+0x0c60/64/68/70): see phy_cal.h. */
/* TOAE_OFDM_CTL +0x0c7c [31:0]: vendor names LNA_LPF_GAIN_LATCH_SELECT,
 * TOD_CAL_MAN_EN, TOD_CAL_EN, TOA_CAL_MAN_EN (bits not tabulated). Live
 * latch: read 0x00100000 then 0 in normal RX. [measured] */
#define MT7996_PHY_TOAE_OFDM_CTL(band)    MT7996_PHY_BN(band, 0x0c7cu)
/* AFE->DFE latency words: OFDM RX0/RX1 +0x0c90/94, OFDM_TOAE_CTL +0x0c98,
 * CCK RX0/RX1 +0x0c9c/a0 (RX1 [13:0]). */
#define MT7996_PHY_TOAE_OFDM_LAT_RX0(band) MT7996_PHY_BN(band, 0x0c90u)
#define MT7996_PHY_TOAE_OFDM_LAT_RX1(band) MT7996_PHY_BN(band, 0x0c94u)
#define MT7996_PHY_TOAE_OFDM_TOAE_CTL(band) MT7996_PHY_BN(band, 0x0c98u)
#define MT7996_PHY_TOAE_CCK_LAT_RX0(band) MT7996_PHY_BN(band, 0x0c9cu)
#define MT7996_PHY_TOAE_CCK_LAT_RX1(band) MT7996_PHY_BN(band, 0x0ca0u)
/*
 * TOAE_OUT_LSB +0x0ca4: ROM names [31] TOAE_OUT_LSB plus unnamed fields
 * [30], [16:14], [12], [9:0]. Rest value 0x40000000 [measured]. The normal
 * WM's CSI dump writes [16:14] = 4 before the bridge dump and 3 after.
 * [static]
 */
#define MT7996_PHY_TOAE_OUT_LSB(band)     MT7996_PHY_BN(band, 0x0ca4u)
#define   MT7996_PHY_TOAE_OUT_CSI_STATE   HW_GENMASK(16, 14)
#define MT7996_PHY_TOAE_OUT_MSB(band)     MT7996_PHY_BN(band, 0x0ca8u)	/* [27:0] */
#define MT7996_PHY_TOD_OUT_LSB(band)      MT7996_PHY_BN(band, 0x0cacu)	/* [27:0] */
#define MT7996_PHY_TOD_OUT_MSB(band)      MT7996_PHY_BN(band, 0x0cb0u)	/* [29:27], [15:0] */
#define MT7996_PHY_TOAE_COARSE_NS_LSB(band) MT7996_PHY_BN(band, 0x0cb4u)
#define MT7996_PHY_TOAE_COARSE_NS_MSB(band) MT7996_PHY_BN(band, 0x0cb8u)
#define MT7996_PHY_TOAE_FINE_TIME(band)   MT7996_PHY_BN(band, 0x0cbcu)	/* [31:23] */
/* TOAE_BYPASS_MODE +0x0cc0: names BYPASS_MODE, CCK_CS_SEL, MEM_ACCESS_MODE,
 * WF_TOAE_SRAM_CS, WF_TOAE_SRAM_A (bits not tabulated). Selects word access
 * to the TOAE SRAM. */
#define MT7996_PHY_TOAE_BYPASS_MODE(band) MT7996_PHY_BN(band, 0x0cc0u)
/* TOAE SRAM word port: SW_TOAE_MEM_DI +0x0cc4 ([28:24], [19:16], [10:0]),
 * SW_TOAE_MEM_DO +0x0ccc ([1:0]). ROM halToaeReadHBuffer walks it. */
#define MT7996_PHY_TOAE_MEM_DI(band)      MT7996_PHY_BN(band, 0x0cc4u)
#define MT7996_PHY_TOAE_MEM_DO(band)      MT7996_PHY_BN(band, 0x0cccu)
/* SW_TOAE_LATCH_DATA_0 +0x0cd0 = RX_BW, _1 +0x0cd4 [10:0] = TAU_M. */
#define MT7996_PHY_TOAE_LATCH0(band)      MT7996_PHY_BN(band, 0x0cd0u)
#define MT7996_PHY_TOAE_LATCH1(band)      MT7996_PHY_BN(band, 0x0cd4u)

/* +0x0d14: [31] set by the RDD2 start: background-receiver front-end enable.
 * [static] */
#define MT7996_PHY_BN_BGRX_EN(band)       MT7996_PHY_BN(band, 0x0d14u)
#define   MT7996_PHY_BN_BGRX_EN_ON        HW_BIT(31)

/*
 * +0x1000: the ROM table names it SW_TOAE_LATCH_DATA_2 ([31] POST_SNR,
 * [30:28] START_ADDR_TOAE, [27:24] POST_RMSD_TOAE, [23:20] POST_SF_IDX, plus
 * unnamed [19:16], [7], [5:4], [3:2], [1], [0]). The WM writes it as a band
 * mode word: the PHY selector programmer puts a code in [27:24] and a mode
 * in [30:28] (code also into RXFE_CTRL[7:4]); WM PHY preparation sets
 * [30:28] to 0 or 1. The channel-set D-die RX setup (V:e00ddd0e, phy_cal.h)
 * writes, one read-modify-write each:
 *  [8] cleared (V:e00de00c), then set at BW320 on the second 320 MHz
 *      channelization (phy_chan.h MT7996_CHSET_320_2);
 *  [27:24] = the BW class itself, 0..4 (one branch per class,
 *      V:e00de088..V:e00de4d6; nothing for classes >= 5): stock 0 / 3 / 4 on
 *      bands 0 / 1 / 2 (BW20 / BW160 / BW320);
 *  [23:20] (POST_SF_IDX) = the primary 20 MHz channel's index inside the
 *      channel (V:e00de19a; the record's [0x10], V:e00e55b0: (ctrl - centre
 *      + 6 / 14 / 30) >> 2 at 80 / 160 / 320 MHz, at 40 MHz 1 when the
 *      primary is above the centre);
 *  [29:28] = the RF band of the record's base frequency (V:e00de1ca /
 *      V:e00de534 / V:e00de562): 0 at 2,407,000 kHz, 1 at 5,000,000 or
 *      4,000,000 kHz, 2 else (6 GHz); [30] and [31] are kept.
 * Stock after the channel set 0x80000000 / 0x93000000 / 0xa4000000 on bands
 * 0 / 1 / 2 ([31] set). [static][measured]
 */
#define MT7996_PHY_BN_MODE(band)          MT7996_PHY_BN(band, 0x1000u)
#define   MT7996_PHY_BN_MODE_MODE         HW_GENMASK(30, 28)
#define   MT7996_PHY_BN_MODE_RF           HW_GENMASK(29, 28)	/* the channel set's view of [30:28] */
#define     MT7996_PHY_BN_MODE_RF_2G4     0u
#define     MT7996_PHY_BN_MODE_RF_5G      1u
#define     MT7996_PHY_BN_MODE_RF_6G      2u
#define   MT7996_PHY_BN_MODE_CODE         HW_GENMASK(27, 24)	/* the channel set: the BW class */
#define   MT7996_PHY_BN_MODE_PRI          HW_GENMASK(23, 20)
#define   MT7996_PHY_BN_MODE_B8           HW_BIT(8)

/* +0x1044: WM PHY register helper sets 0x20 then 0x30 (modes 5, 12) and
 * [0] then [1] (modes 0, 1, 2, 4, 5, 12, 20, 23). Not decoded. [static] */
#define MT7996_PHY_BN_1044(band)          MT7996_PHY_BN(band, 0x1044u)

/* MAC-facing PHYCTRL / MU registers (no RX front-end role). [static: ROM
 * names; mt76 uses GID_TAB_VLD/POS]
 * PHYCTRL_RATE_CTRL +0x104c: HT/VHT NDP and NSS checks.
 * PHYCTRL_GIDTAB +0x1050: [31] TEST_MODE, [24] TEST_UPD, [21:16] TEST_GID.
 * GID_TAB_VLD_0/1 +0x1054/58, GID_TAB_POS_0..3 +0x105c..+0x1068.
 * HEMU_RULE1..4 +0x10e0..+0x10ec: [30:28] PRIORITY, [26] UDL_EN, [25]
 * BSS_EN, [24] AID_EN, [22] UDL, [21:16] BSS, [10:0] AID. The PHY-init
 * routine's tail sets HEMU_RULE4 (n = 3) PRIORITY = 1, then UDL_EN with UDL
 * = 1 (0x14400000), on all three bands (V:e00edaa0 -> V:e00ced9e, one
 * field per call: AID with AID_EN, BSS with BSS_EN, UDL with UDL_EN, or
 * PRIORITY). [static][measured] */
#define MT7996_PHY_PHYCTRL_RATE_CTRL(band) MT7996_PHY_BN(band, 0x104cu)
#define MT7996_PHY_PHYCTRL_GIDTAB(band)   MT7996_PHY_BN(band, 0x1050u)
#define   MT7996_PHY_PHYCTRL_GIDTAB_TEST_MODE HW_BIT(31)
#define MT7996_PHY_GID_TAB_VLD(band, n)   MT7996_PHY_BN(band, 0x1054u + 4u * (n))
#define MT7996_PHY_GID_TAB_POS(band, n)   MT7996_PHY_BN(band, 0x105cu + 4u * (n))
#define MT7996_PHY_HEMU_RULE(band, n)     MT7996_PHY_BN(band, 0x10e0u + 4u * (n))
#define   MT7996_PHY_HEMU_RULE_PRIORITY   HW_GENMASK(30, 28)
#define   MT7996_PHY_HEMU_RULE_UDL_EN     HW_BIT(26)
#define   MT7996_PHY_HEMU_RULE_BSS_EN     HW_BIT(25)
#define   MT7996_PHY_HEMU_RULE_AID_EN     HW_BIT(24)
#define   MT7996_PHY_HEMU_RULE_UDL        HW_BIT(22)
#define   MT7996_PHY_HEMU_RULE_BSS        HW_GENMASK(21, 16)
#define   MT7996_PHY_HEMU_RULE_AID        HW_GENMASK(10, 0)

/* +0x10a4 (PHYCTRL) and its twin +0x110a4: the per-band PHY init sets [3:0]
 * = 4 (observed 0x14 / 0 -> 0x14 / 4). Not decoded. [static][measured] */
#define MT7996_PHY_BN_10A4(band)          MT7996_PHY_BN(band, 0x10a4u)
#define MT7996_PHY_BN2_10A4(band)         MT7996_PHY_BN2(band, 0x10a4u)
#define   MT7996_PHY_BN_10A4_LO           HW_GENMASK(3, 0)
#define   MT7996_PHY_BN_10A4_LO_INIT      4u	/* e00def4e / e00def68 */

/*
 * PHYINTF_RX_CTRL_1, +0x2004, RW, 0 in normal RX. [measured] Per-function
 * 3-bit enables. mt76 drives IPI_EN and STSCNT_EN; the RDD2 start sets
 * IPI_EN = 5 on first use. [static]
 */
#define MT7996_PHY_RX_CTRL1(band)         MT7996_PHY_BN(band, 0x2004u)
#define   MT7996_PHY_RX_CTRL1_MDRDY       HW_GENMASK(31, 30)
#define   MT7996_PHY_RX_CTRL1_STSCNT_EN   HW_GENMASK(11, 9)
#define   MT7996_PHY_RX_CTRL1_RDD_EN      HW_GENMASK(8, 6)
#define   MT7996_PHY_RX_CTRL1_RPI_EN      HW_GENMASK(5, 3)
#define   MT7996_PHY_RX_CTRL1_IPI_EN      HW_GENMASK(2, 0)

/* BN_PHYINTF +0x2aa0..+0x2aa8 and +0x2b00..+0x2b10: whole words written by
 * the per-band PHY init (write only): 0xd000fff0, 0x5555be55, 0x55555555;
 * 0xd0002000, 0x55555555 x 3, 0xafbb5555. [static] [guess: 2-bit-per-element
 * pattern words] */
#define MT7996_PHY_BN_PHYINTF_2AA0(band, n) MT7996_PHY_BN(band, 0x2aa0u + 4u * (n))	/* n 0..2 */
#define MT7996_PHY_BN_PHYINTF_2B00(band, n) MT7996_PHY_BN(band, 0x2b00u + 4u * (n))	/* n 0..4 */
#define MT7996_PHY_BN_PHYINTF_2AA0_INIT   { 0xd000fff0u, 0x5555be55u, 0x55555555u }
#define MT7996_PHY_BN_PHYINTF_2B00_INIT   { 0xd0002000u, 0x55555555u, 0x55555555u, 0x55555555u, 0xafbb5555u }

/* Beamforming / MU-TX control (PHYRX_CTRL_BN_BF/MULQ/MURU). [static: ROM]
 * BF_CONTROL +0x5000: [3]/[1] band1/band0 RX iBF enable, [2]/[0] eBF enable,
 * [4] DBDC band select, force-angle/CQI and BF-off controls in [21:8].
 * MUTX_CONTROL_0 +0x5230: [26] ITXBF_EN, [25] ETXBF_EN, [24] MUTXBF_EN.
 * MUTX_LQ_0..3 +0x5234..+0x5240, MUTX_PN +0x5244. */
#define MT7996_PHY_BF_CONTROL(band)       MT7996_PHY_BN(band, 0x5000u)
#define MT7996_PHY_MUTX_CONTROL0(band)    MT7996_PHY_BN(band, 0x5230u)
#define MT7996_PHY_MUTX_LQ(band, n)       MT7996_PHY_BN(band, 0x5234u + 4u * (n))
#define MT7996_PHY_MUTX_PN(band)          MT7996_PHY_BN(band, 0x5244u)

/* +0x6094 and its twin +0x16094: the per-band PHY init sets [21:20] = 2
 * (observed 0xca300600 / 0 -> 0xca200600 / 0x00200000). Block +0x6000 is in
 * no ROM map; the init's register table writes 10 more words there
 * (phy_map.h MT7996_PHY_REGTBL_*). [static][measured] */
#define MT7996_PHY_BN_6094(band)          MT7996_PHY_BN(band, 0x6094u)
#define MT7996_PHY_BN2_6094(band)         MT7996_PHY_BN2(band, 0x6094u)
#define   MT7996_PHY_BN_6094_F21_20       HW_GENMASK(21, 20)
#define   MT7996_PHY_BN_6094_F21_20_INIT  2u	/* e00def02 */
/* +0x6098: [0] = 1 by the channel-set D-die RX calibration on every band
 * (0x03030300 -> 0x03030301). Not decoded. [static][measured] */
#define MT7996_PHY_BN_6098(band)          MT7996_PHY_BN(band, 0x6098u)
#define   MT7996_PHY_BN_6098_B0           HW_BIT(0)
/* +0x6188 [name guess: a bandwidth word]: written by every channel set
 * (V:e00d188e, phy_cal.h "Channel set") as [11:0], then [27:16] (two
 * read-modify-writes), both the same bandwidth value round(N / f_MHz)
 * (phy_chan.h MT7996_CHSET_6188_NUM): 0x29f (BW20), 0x138 (BW160), 0x10c
 * (BW320); 0x00ea00ea after the per-band PHY init. In the receive-only
 * subset. Followed by MT7996_PHY_DFE_F168. PHY table B (phy_map.h) writes
 * 15 further words of the block (+0x60b0..+0x62c8) at every channel set.
 * [static][measured] */
#define MT7996_PHY_BN_6188(band)          MT7996_PHY_BN(band, 0x6188u)
#define   MT7996_PHY_BN_6188_HI           HW_GENMASK(27, 16)
#define   MT7996_PHY_BN_6188_LO           HW_GENMASK(11, 0)

/* RXTD12, +0x8230: IRPI histogram clear. [static: mt76] The per-band PHY
 * init's register table writes 0x9000c2c2. [static][measured] */
#define MT7996_PHY_RXTD12(band)           MT7996_PHY_BN(band, 0x8230u)
#define   MT7996_PHY_RXTD12_IRPI_SW_CLR_ONLY HW_BIT(18)
#define   MT7996_PHY_RXTD12_IRPI_SW_CLR   HW_BIT(29)

/*
 * RXTD band words with known writers, no names. [static]
 *  +0x8014 [27], [25]; +0x8214 [14:13]; +0x823c [23]: SW-LNA profile fields.
 *  +0x8200 [21], [29]; +0x8550 [1]; +0x8d60 [29]: cleared (saved, then
 *    restored) by the vendor ICAP manual-gain recipe. The channel-set D-die
 *    RX setup writes +0x8200 [21] by band: set on 2.4 GHz (base 2,407,000
 *    kHz), cleared on 5 / 6 GHz (V:e00de1e6..; stock unchanged).
 *  +0x8238 [15]: read-modified by the channel-set D-die RX calibration
 *    (0x6920a8e0 unchanged).
 *  +0x8554 [7:0]: SW-LNA profile field; with [23:16], [15:8] and +0x8d08
 *    [7:0] an EDCCA threshold [inferred] (MT7996_PHY_BN_8554, below).
 *  +0x8560 [27]: SW-LNA profile strobe (set, then cleared).
 * The per-band PHY init's register table writes +0x8238 = 0x6920a8e0 and
 * +0x8550 = 0x0698000a, and the init itself (below): [static][measured]
 *  +0x8234 [0] = 0 (0x03100001 -> 0x03100000);
 *  +0x8240 [23:19] = 0 (0xe1003000 unchanged);
 *  +0x8590, +0x85a0 [15] = 0, [14] = 0 (0x0cabf222 -> 0x0cab3222). At
 *    every channel set PHY table B writes +0x85a0 whole, then its explicit
 *    field (V:e00d97d2 / V:e00d982a; skipped when MT7996_CBTOP_HW_REV ==
 *    0x8a00): [15:14] = 3 on 6 GHz (key k0 = 2) at BW class >= 2, else 0
 *    (stock band 2 0x0cabf222, bands 0, 1 0x0cab3222; phy_map.h).
 */
#define MT7996_PHY_BN_8014(band)          MT7996_PHY_BN(band, 0x8014u)
#define MT7996_PHY_BN_8200(band)          MT7996_PHY_BN(band, 0x8200u)
#define   MT7996_PHY_BN_8200_B21          HW_BIT(21)
#define   MT7996_PHY_BN_8200_B29          HW_BIT(29)
#define MT7996_PHY_BN_8214(band)          MT7996_PHY_BN(band, 0x8214u)
#define MT7996_PHY_BN_8238(band)          MT7996_PHY_BN(band, 0x8238u)
#define   MT7996_PHY_BN_8238_B15          HW_BIT(15)	/* set by the channel-set RX setup (V:e00de074); stock 0x6920a8e0, unchanged */
#define MT7996_PHY_BN_823C(band)          MT7996_PHY_BN(band, 0x823cu)
#define MT7996_PHY_BN_8550(band)          MT7996_PHY_BN(band, 0x8550u)
#define   MT7996_PHY_BN_8550_B1           HW_BIT(1)
#define MT7996_PHY_BN_8554(band)          MT7996_PHY_BN(band, 0x8554u)
/* +0x8554 [23:16], [15:8], [7:0] and +0x8d08 [7:0]: four 8-bit EDCCA thresholds
 * [inferred from MediaTek's log strings: V:e00afefc, "No EDCCA TH value from
 * wifi.cfg SKIP setting"; stock band 0 0xd8c5c2c1 / 0x00d4d4c9, i.e. -59 / -62 /
 * -63 and -55 as signed bytes, dBm]. Every channel set reads and writes them back
 * (V:e00afca2, phy_chan.h MT7996_CHSET_EDCCA_*; [31:24] and +0x8d08 [31:8] kept).
 * [static][measured] */
#define   MT7996_PHY_BN_8554_F2           HW_GENMASK(23, 16)
#define   MT7996_PHY_BN_8554_F1           HW_GENMASK(15, 8)
#define   MT7996_PHY_BN_8554_F0           HW_GENMASK(7, 0)
#define   MT7996_PHY_BN_8554_KEEP         HW_GENMASK(31, 24)
#define MT7996_PHY_BN_SWLNA_STROBE(band)  MT7996_PHY_BN(band, 0x8560u)
#define   MT7996_PHY_BN_SWLNA_STROBE_B27  HW_BIT(27)
#define MT7996_PHY_BN_8D60(band)          MT7996_PHY_BN(band, 0x8d60u)
#define   MT7996_PHY_BN_8D60_B29          HW_BIT(29)
#define MT7996_PHY_BN_8234(band)          MT7996_PHY_BN(band, 0x8234u)
#define   MT7996_PHY_BN_8234_B0           HW_BIT(0)
#define MT7996_PHY_BN_8240(band)          MT7996_PHY_BN(band, 0x8240u)
#define   MT7996_PHY_BN_8240_F23_19       HW_GENMASK(23, 19)
#define MT7996_PHY_BN_8590(band)          MT7996_PHY_BN(band, 0x8590u)
#define MT7996_PHY_BN_85A0(band)          MT7996_PHY_BN(band, 0x85a0u)
#define   MT7996_PHY_BN_85X0_B15          HW_BIT(15)
#define   MT7996_PHY_BN_85X0_B14          HW_BIT(14)
#define   MT7996_PHY_BN_85A0_F15_14       HW_GENMASK(15, 14)	/* +0x85a0 [15:14] as PHY table B's field */

/*
 * BN_RXTD +0x8e30 and the coefficient words +0x8e60..+0x8e74 exist on band 2
 * only: on bands 0 and 1 every read returns 0, also right after a nonzero
 * write (V:e00ded2c, V:e00ded64, V:e00dedc0, V:e00dee18); band 2 reads
 * 0x0b020000 and 0x03c703bf, 0x03bf0000, 0x03830373, 0x03730000,
 * 0x031702f7, 0x02f70000 before the per-band PHY init. [measured]
 * +0x8e30: the init sets [30] = 1, [29] = 0 (0x0b020000 -> 0x4b020000); E1
 *   parts also clear [31]. At every channel set PHY table B writes it
 *   whole, then [31:30] = 3 under the +0x85a0 condition above, else 0
 *   (V:e00d97ea / V:e00d9844; stock band 2 0xcb020000).
 * +0x8e60 + 4 n, n 0..5: two signed 10-bit fields, HI [25:16] and LO [9:0].
 *   V:e00de622 sets e60 HI / LO = 0x3fd / 0x007, e64 HI = 0x007, e68 =
 *   0x3b9 / 0x021, e6c HI = 0x021, e70 = 0x34d / 0x01d, e74 HI = 0x01d; the
 *   absolute register table (rows 286..291, the same pass) then overwrites
 *   all six whole words with MT7996_PHY_BN_RXTD_COEF_TBL, which is in force.
 *   Each odd word's HI equals the even word's LO before it.
 * [static][measured] [guess: RX time-domain filter taps of the 320 MHz band]
 */
#define MT7996_PHY_BN_8E30(band)          MT7996_PHY_BN(band, 0x8e30u)
#define   MT7996_PHY_BN_8E30_B31          HW_BIT(31)	/* E1 only */
#define   MT7996_PHY_BN_8E30_B30          HW_BIT(30)
#define   MT7996_PHY_BN_8E30_B29          HW_BIT(29)
#define   MT7996_PHY_BN_8E30_F31_30       HW_GENMASK(31, 30)	/* PHY table B's field */
#define MT7996_PHY_BN_RXTD_COEF(band, n)  MT7996_PHY_BN(band, 0x8e60u + 4u * (n))	/* n 0..5 */
#define   MT7996_PHY_BN_RXTD_COEF_HI      HW_GENMASK(25, 16)
#define   MT7996_PHY_BN_RXTD_COEF_LO      HW_GENMASK(9, 0)
#define MT7996_PHY_BN_RXTD_COEF_TBL       { 0x03fc03f4u, 0x03f40000u, 0x03b303a3u, \
					    0x03a30000u, 0x03600340u, 0x03400000u }
/* The per-band init's field writes of the six words (n 0..5): HI of
 * every word, LO of the even ones (e00ded4c..e00dee34); the register table
 * rewrites all six whole words later in the same pass. */
#define MT7996_PHY_BN_RXTD_COEF_N         6u
#define MT7996_PHY_BN_RXTD_COEF_HI_INIT   { 0x3fdu, 0x007u, 0x3b9u, 0x021u, 0x34du, 0x01du }
#define MT7996_PHY_BN_RXTD_COEF_LO_INIT   { 0x007u, 0u, 0x021u, 0u, 0x01du, 0u }	/* even n only */

/* +0xa004..+0xa054, +0xb000 [31:30], +0xb004: the band test-signal generator
 * of the OTFIQ calibration and of DPD's TX power sweep (phy_cal.h
 * MT7996_PHY_SIGGEN_START / _CFG). [static] */

/* Offset names, role unknown unless a header says more: rows of the per-band PHY init's
 * register tables (phy_init.h MT7996_PHY_INIT_REGTBL_*; "init", written whole on every
 * band, never read by the init) and of the channel-set PHY tables A / B (phy_chan.h
 * MT7996_CHSET_TBL_A_ROWS / _B_ROWS; "table A" / "table B"), and words the channel set
 * writes by field. [static][measured]
 * Sub-blocks: +0x007c BN_CTRL; +0x5a00..+0x5aac BN_BF (beamforming); +0x6000..
 * (no map entry); +0x8000.. BN_RXTD (RX time domain); +0xb000 BN_B000. */
#define MT7996_PHY_BN_007C(band)           MT7996_PHY_BN(band, 0x007cu)	/* init */
#define MT7996_PHY_BN_5A00(band)           MT7996_PHY_BN(band, 0x5a00u)	/* init */
#define MT7996_PHY_BN_5A14(band)           MT7996_PHY_BN(band, 0x5a14u)	/* init */
#define MT7996_PHY_BN_5A18(band)           MT7996_PHY_BN(band, 0x5a18u)	/* init */
#define MT7996_PHY_BN_5A1C(band)           MT7996_PHY_BN(band, 0x5a1cu)	/* init */
#define MT7996_PHY_BN_5A20(band)           MT7996_PHY_BN(band, 0x5a20u)	/* init */
#define MT7996_PHY_BN_5A24(band)           MT7996_PHY_BN(band, 0x5a24u)	/* init */
#define MT7996_PHY_BN_5A28(band)           MT7996_PHY_BN(band, 0x5a28u)	/* init */
#define MT7996_PHY_BN_5A2C(band)           MT7996_PHY_BN(band, 0x5a2cu)	/* init */
#define MT7996_PHY_BN_5A30(band)           MT7996_PHY_BN(band, 0x5a30u)	/* init */
#define MT7996_PHY_BN_5A34(band)           MT7996_PHY_BN(band, 0x5a34u)	/* init */
#define MT7996_PHY_BN_5A38(band)           MT7996_PHY_BN(band, 0x5a38u)	/* init */
#define MT7996_PHY_BN_5A3C(band)           MT7996_PHY_BN(band, 0x5a3cu)	/* init */
#define MT7996_PHY_BN_5A40(band)           MT7996_PHY_BN(band, 0x5a40u)	/* init */
#define MT7996_PHY_BN_5A44(band)           MT7996_PHY_BN(band, 0x5a44u)	/* init */
#define MT7996_PHY_BN_5A48(band)           MT7996_PHY_BN(band, 0x5a48u)	/* init */
#define MT7996_PHY_BN_5A4C(band)           MT7996_PHY_BN(band, 0x5a4cu)	/* init */
#define MT7996_PHY_BN_5A50(band)           MT7996_PHY_BN(band, 0x5a50u)	/* init */
#define MT7996_PHY_BN_5AA8(band)           MT7996_PHY_BN(band, 0x5aa8u)	/* init */
#define MT7996_PHY_BN_5AAC(band)           MT7996_PHY_BN(band, 0x5aacu)	/* init */
#define MT7996_PHY_BN_60B0(band)           MT7996_PHY_BN(band, 0x60b0u)	/* table B */
#define MT7996_PHY_BN_60B4(band)           MT7996_PHY_BN(band, 0x60b4u)	/* init */
#define MT7996_PHY_BN_60BC(band)           MT7996_PHY_BN(band, 0x60bcu)	/* init */
#define MT7996_PHY_BN_60C4(band)           MT7996_PHY_BN(band, 0x60c4u)	/* table B */
#define MT7996_PHY_BN_60C8(band)           MT7996_PHY_BN(band, 0x60c8u)	/* table B */
#define MT7996_PHY_BN_60E4(band)           MT7996_PHY_BN(band, 0x60e4u)	/* init */
#define MT7996_PHY_BN_60F8(band)           MT7996_PHY_BN(band, 0x60f8u)	/* init */
#define MT7996_PHY_BN_60FC(band)           MT7996_PHY_BN(band, 0x60fcu)	/* init */
#define MT7996_PHY_BN_6100(band)           MT7996_PHY_BN(band, 0x6100u)	/* init */
#define MT7996_PHY_BN_6104(band)           MT7996_PHY_BN(band, 0x6104u)	/* init */
#define MT7996_PHY_BN_6138(band)           MT7996_PHY_BN(band, 0x6138u)	/* table B */
#define MT7996_PHY_BN_6148(band)           MT7996_PHY_BN(band, 0x6148u)	/* init */
#define MT7996_PHY_BN_614C(band)           MT7996_PHY_BN(band, 0x614cu)	/* init */
#define MT7996_PHY_BN_62A0(band)           MT7996_PHY_BN(band, 0x62a0u)	/* table B */
#define MT7996_PHY_BN_62A4(band)           MT7996_PHY_BN(band, 0x62a4u)	/* table B */
#define MT7996_PHY_BN_62A8(band)           MT7996_PHY_BN(band, 0x62a8u)	/* table B */
#define MT7996_PHY_BN_62AC(band)           MT7996_PHY_BN(band, 0x62acu)	/* table B */
#define MT7996_PHY_BN_62B0(band)           MT7996_PHY_BN(band, 0x62b0u)	/* table B */
#define MT7996_PHY_BN_62B4(band)           MT7996_PHY_BN(band, 0x62b4u)	/* table B */
#define MT7996_PHY_BN_62B8(band)           MT7996_PHY_BN(band, 0x62b8u)	/* table B */
#define MT7996_PHY_BN_62BC(band)           MT7996_PHY_BN(band, 0x62bcu)	/* table B */
#define MT7996_PHY_BN_62C0(band)           MT7996_PHY_BN(band, 0x62c0u)	/* table B */
#define MT7996_PHY_BN_62C4(band)           MT7996_PHY_BN(band, 0x62c4u)	/* table B */
#define MT7996_PHY_BN_62C8(band)           MT7996_PHY_BN(band, 0x62c8u)	/* table B */
#define MT7996_PHY_BN_64A8(band)           MT7996_PHY_BN(band, 0x64a8u)	/* init */
#define MT7996_PHY_BN_8010(band)           MT7996_PHY_BN(band, 0x8010u)	/* table B */
#define MT7996_PHY_BN_8018(band)           MT7996_PHY_BN(band, 0x8018u)	/* table B */
#define MT7996_PHY_BN_8020(band)           MT7996_PHY_BN(band, 0x8020u)	/* init */
#define MT7996_PHY_BN_8204(band)           MT7996_PHY_BN(band, 0x8204u)	/* table B */
#define MT7996_PHY_BN_8208(band)           MT7996_PHY_BN(band, 0x8208u)	/* table B */
#define MT7996_PHY_BN_820C(band)           MT7996_PHY_BN(band, 0x820cu)	/* table B */
#define MT7996_PHY_BN_8210(band)           MT7996_PHY_BN(band, 0x8210u)	/* table B */
#define MT7996_PHY_BN_8218(band)           MT7996_PHY_BN(band, 0x8218u)	/* init */
#define MT7996_PHY_BN_8244(band)           MT7996_PHY_BN(band, 0x8244u)	/* init */
#define MT7996_PHY_BN_8248(band)           MT7996_PHY_BN(band, 0x8248u)	/* init */
#define MT7996_PHY_BN_824C(band)           MT7996_PHY_BN(band, 0x824cu)	/* init */
#define MT7996_PHY_BN_825C(band)           MT7996_PHY_BN(band, 0x825cu)	/* table B */
#define MT7996_PHY_BN_8264(band)           MT7996_PHY_BN(band, 0x8264u)	/* init */
#define MT7996_PHY_BN_8268(band)           MT7996_PHY_BN(band, 0x8268u)	/* init */
#define MT7996_PHY_BN_826C(band)           MT7996_PHY_BN(band, 0x826cu)	/* init */
#define MT7996_PHY_BN_829C(band)           MT7996_PHY_BN(band, 0x829cu)	/* table B */
#define MT7996_PHY_BN_82BC(band)           MT7996_PHY_BN(band, 0x82bcu)	/* table B */
#define MT7996_PHY_BN_82D0(band)           MT7996_PHY_BN(band, 0x82d0u)	/* table B */
#define MT7996_PHY_BN_82D4(band)           MT7996_PHY_BN(band, 0x82d4u)	/* table B */
#define MT7996_PHY_BN_82D8(band)           MT7996_PHY_BN(band, 0x82d8u)	/* table B */
#define MT7996_PHY_BN_82DC(band)           MT7996_PHY_BN(band, 0x82dcu)	/* table B */
#define MT7996_PHY_BN_82E0(band)           MT7996_PHY_BN(band, 0x82e0u)	/* table B */
#define MT7996_PHY_BN_82E4(band)           MT7996_PHY_BN(band, 0x82e4u)	/* init */
#define MT7996_PHY_BN_82F8(band)           MT7996_PHY_BN(band, 0x82f8u)	/* table B */
#define MT7996_PHY_BN_8300(band)           MT7996_PHY_BN(band, 0x8300u)	/* table B */
#define MT7996_PHY_BN_8304(band)           MT7996_PHY_BN(band, 0x8304u)	/* table B */
#define MT7996_PHY_BN_8308(band)           MT7996_PHY_BN(band, 0x8308u)	/* table B */
#define MT7996_PHY_BN_830C(band)           MT7996_PHY_BN(band, 0x830cu)	/* table B */
#define MT7996_PHY_BN_8324(band)           MT7996_PHY_BN(band, 0x8324u)	/* table B */
#define MT7996_PHY_BN_8380(band)           MT7996_PHY_BN(band, 0x8380u)	/* table B */
#define MT7996_PHY_BN_838C(band)           MT7996_PHY_BN(band, 0x838cu)	/* table B */
#define MT7996_PHY_BN_8394(band)           MT7996_PHY_BN(band, 0x8394u)	/* init */
#define MT7996_PHY_BN_8398(band)           MT7996_PHY_BN(band, 0x8398u)	/* init */
#define MT7996_PHY_BN_83A0(band)           MT7996_PHY_BN(band, 0x83a0u)	/* init */
#define MT7996_PHY_BN_83A4(band)           MT7996_PHY_BN(band, 0x83a4u)	/* init */
#define MT7996_PHY_BN_83AC(band)           MT7996_PHY_BN(band, 0x83acu)	/* init */
#define MT7996_PHY_BN_83B0(band)           MT7996_PHY_BN(band, 0x83b0u)	/* init */
#define MT7996_PHY_BN_83B4(band)           MT7996_PHY_BN(band, 0x83b4u)	/* table B */
#define MT7996_PHY_BN_83B8(band)           MT7996_PHY_BN(band, 0x83b8u)	/* table B */
#define MT7996_PHY_BN_83BC(band)           MT7996_PHY_BN(band, 0x83bcu)	/* init */
#define MT7996_PHY_BN_83C0(band)           MT7996_PHY_BN(band, 0x83c0u)	/* table B */
#define MT7996_PHY_BN_83C4(band)           MT7996_PHY_BN(band, 0x83c4u)	/* init */
#define MT7996_PHY_BN_83C8(band)           MT7996_PHY_BN(band, 0x83c8u)	/* table B */
#define MT7996_PHY_BN_83F0(band)           MT7996_PHY_BN(band, 0x83f0u)	/* table B */
#define MT7996_PHY_BN_83F4(band)           MT7996_PHY_BN(band, 0x83f4u)	/* table B */
#define MT7996_PHY_BN_84A0(band)           MT7996_PHY_BN(band, 0x84a0u)	/* init */
#define MT7996_PHY_BN_84A4(band)           MT7996_PHY_BN(band, 0x84a4u)	/* init */
#define MT7996_PHY_BN_84C0(band)           MT7996_PHY_BN(band, 0x84c0u)	/* init */
#define MT7996_PHY_BN_84F0(band)           MT7996_PHY_BN(band, 0x84f0u)	/* init */
#define MT7996_PHY_BN_8900(band)           MT7996_PHY_BN(band, 0x8900u)	/* table B */
#define MT7996_PHY_BN_8910(band)           MT7996_PHY_BN(band, 0x8910u)	/* table B */
#define MT7996_PHY_BN_8914(band)           MT7996_PHY_BN(band, 0x8914u)	/* table B */
#define MT7996_PHY_BN_8918(band)           MT7996_PHY_BN(band, 0x8918u)	/* table B */
#define MT7996_PHY_BN_8920(band)           MT7996_PHY_BN(band, 0x8920u)	/* table B */
#define MT7996_PHY_BN_8924(band)           MT7996_PHY_BN(band, 0x8924u)	/* table B */
#define MT7996_PHY_BN_8928(band)           MT7996_PHY_BN(band, 0x8928u)	/* table B */
#define MT7996_PHY_BN_8930(band)           MT7996_PHY_BN(band, 0x8930u)	/* table B */
#define MT7996_PHY_BN_8934(band)           MT7996_PHY_BN(band, 0x8934u)	/* table B */
#define MT7996_PHY_BN_8938(band)           MT7996_PHY_BN(band, 0x8938u)	/* table B */
#define MT7996_PHY_BN_8940(band)           MT7996_PHY_BN(band, 0x8940u)	/* table B */
#define MT7996_PHY_BN_8944(band)           MT7996_PHY_BN(band, 0x8944u)	/* table B */
#define MT7996_PHY_BN_8948(band)           MT7996_PHY_BN(band, 0x8948u)	/* table B */
#define MT7996_PHY_BN_8950(band)           MT7996_PHY_BN(band, 0x8950u)	/* table B */
#define MT7996_PHY_BN_8954(band)           MT7996_PHY_BN(band, 0x8954u)	/* table B */
#define MT7996_PHY_BN_8958(band)           MT7996_PHY_BN(band, 0x8958u)	/* table B */
#define MT7996_PHY_BN_8960(band)           MT7996_PHY_BN(band, 0x8960u)	/* table B */
#define MT7996_PHY_BN_8964(band)           MT7996_PHY_BN(band, 0x8964u)	/* table B */
#define MT7996_PHY_BN_8968(band)           MT7996_PHY_BN(band, 0x8968u)	/* table B */
#define MT7996_PHY_BN_8970(band)           MT7996_PHY_BN(band, 0x8970u)	/* table B */
#define MT7996_PHY_BN_8974(band)           MT7996_PHY_BN(band, 0x8974u)	/* table B */
#define MT7996_PHY_BN_8978(band)           MT7996_PHY_BN(band, 0x8978u)	/* table B */
#define MT7996_PHY_BN_8980(band)           MT7996_PHY_BN(band, 0x8980u)	/* table B */
#define MT7996_PHY_BN_8984(band)           MT7996_PHY_BN(band, 0x8984u)	/* table B */
#define MT7996_PHY_BN_8988(band)           MT7996_PHY_BN(band, 0x8988u)	/* table B */
#define MT7996_PHY_BN_8998(band)           MT7996_PHY_BN(band, 0x8998u)	/* init */
#define MT7996_PHY_BN_89A0(band)           MT7996_PHY_BN(band, 0x89a0u)	/* init */
#define MT7996_PHY_BN_89B0(band)           MT7996_PHY_BN(band, 0x89b0u)	/* init */
#define MT7996_PHY_BN_8C08(band)           MT7996_PHY_BN(band, 0x8c08u)	/* init */
#define MT7996_PHY_BN_8C0C(band)           MT7996_PHY_BN(band, 0x8c0cu)	/* init */
#define MT7996_PHY_BN_8C10(band)           MT7996_PHY_BN(band, 0x8c10u)	/* init */
#define MT7996_PHY_BN_8D00(band)           MT7996_PHY_BN(band, 0x8d00u)	/* table B */
#define MT7996_PHY_BN_8D04(band)           MT7996_PHY_BN(band, 0x8d04u)	/* table B */

/* +0x8d08 [7:0]: the fourth EDCCA threshold (MT7996_PHY_BN_8554). */
#define MT7996_PHY_BN_8D08(band)           MT7996_PHY_BN(band, 0x8d08u)	/* table B */
#define   MT7996_PHY_BN_8D08_F0           HW_GENMASK(7, 0)

#define MT7996_PHY_BN_8D68(band)           MT7996_PHY_BN(band, 0x8d68u)	/* table B */
#define MT7996_PHY_BN_8D70(band)           MT7996_PHY_BN(band, 0x8d70u)	/* table B */
#define MT7996_PHY_BN_8D74(band)           MT7996_PHY_BN(band, 0x8d74u)	/* table B */
#define MT7996_PHY_BN_8D78(band)           MT7996_PHY_BN(band, 0x8d78u)	/* table B */
#define MT7996_PHY_BN_8D7C(band)           MT7996_PHY_BN(band, 0x8d7cu)	/* table B */
#define MT7996_PHY_BN_8D80(band)           MT7996_PHY_BN(band, 0x8d80u)	/* table B */
#define MT7996_PHY_BN_8D84(band)           MT7996_PHY_BN(band, 0x8d84u)	/* table B */
#define MT7996_PHY_BN_8D90(band)           MT7996_PHY_BN(band, 0x8d90u)	/* init */
#define MT7996_PHY_BN_8D94(band)           MT7996_PHY_BN(band, 0x8d94u)	/* init */
#define MT7996_PHY_BN_8D98(band)           MT7996_PHY_BN(band, 0x8d98u)	/* init */
#define MT7996_PHY_BN_8D9C(band)           MT7996_PHY_BN(band, 0x8d9cu)	/* init */
#define MT7996_PHY_BN_8DA0(band)           MT7996_PHY_BN(band, 0x8da0u)	/* init */
#define MT7996_PHY_BN_8DA4(band)           MT7996_PHY_BN(band, 0x8da4u)	/* init */
#define MT7996_PHY_BN_8DA8(band)           MT7996_PHY_BN(band, 0x8da8u)	/* init */
#define MT7996_PHY_BN_8DAC(band)           MT7996_PHY_BN(band, 0x8dacu)	/* init */
#define MT7996_PHY_BN_8DB0(band)           MT7996_PHY_BN(band, 0x8db0u)	/* init */
#define MT7996_PHY_BN_8DB4(band)           MT7996_PHY_BN(band, 0x8db4u)	/* init */
#define MT7996_PHY_BN_8DC0(band)           MT7996_PHY_BN(band, 0x8dc0u)	/* init */
#define MT7996_PHY_BN_8DC4(band)           MT7996_PHY_BN(band, 0x8dc4u)	/* init */
#define MT7996_PHY_BN_8DC8(band)           MT7996_PHY_BN(band, 0x8dc8u)	/* init */
#define MT7996_PHY_BN_8E08(band)           MT7996_PHY_BN(band, 0x8e08u)	/* init */
#define MT7996_PHY_BN_8E14(band)           MT7996_PHY_BN(band, 0x8e14u)	/* table B */
#define MT7996_PHY_BN_8E18(band)           MT7996_PHY_BN(band, 0x8e18u)	/* table B */
#define MT7996_PHY_BN_8E1C(band)           MT7996_PHY_BN(band, 0x8e1cu)	/* table B */
#define MT7996_PHY_BN_8E20(band)           MT7996_PHY_BN(band, 0x8e20u)	/* table B */
#define MT7996_PHY_BN_8E24(band)           MT7996_PHY_BN(band, 0x8e24u)	/* table B */

/* BN +0xb000 [name guess: TX stream count word]: V:e00de03c writes [31:30], then V:e00de054
 * [27:26], each = the record's [0x12] (ctx->nss_tx) - 1, one read-modify-write each
 * (MediaTek ORs the shifted value unmasked). Stock 0xcc000000 (4 streams), unchanged. Role
 * unknown (phy_map.h block BN_B000). [static][measured] */
#define MT7996_PHY_BN_B000(band)           MT7996_PHY_BN(band, 0xb000u)
#define   MT7996_PHY_BN_B000_F31_30       HW_GENMASK(31, 30)
#define   MT7996_PHY_BN_B000_F27_26       HW_GENMASK(27, 26)

#define MT7996_PHY_BN_D01C(band)           MT7996_PHY_BN(band, 0xd01cu)	/* table A */
#define MT7996_PHY_BN_D020(band)           MT7996_PHY_BN(band, 0xd020u)	/* table A */
#define MT7996_PHY_BN_D024(band)           MT7996_PHY_BN(band, 0xd024u)	/* table A */
#define MT7996_PHY_BN_D028(band)           MT7996_PHY_BN(band, 0xd028u)	/* table A */
#define MT7996_PHY_BN_D02C(band)           MT7996_PHY_BN(band, 0xd02cu)	/* table A */
#define MT7996_PHY_BN_D030(band)           MT7996_PHY_BN(band, 0xd030u)	/* table A */
#define MT7996_PHY_BN_D034(band)           MT7996_PHY_BN(band, 0xd034u)	/* table A */
#define MT7996_PHY_BN_D038(band)           MT7996_PHY_BN(band, 0xd038u)	/* table A */
#define MT7996_PHY_BN_D03C(band)           MT7996_PHY_BN(band, 0xd03cu)	/* table A */
#define MT7996_PHY_BN_D040(band)           MT7996_PHY_BN(band, 0xd040u)	/* table A */
#define MT7996_PHY_BN_D044(band)           MT7996_PHY_BN(band, 0xd044u)	/* table A */
#define MT7996_PHY_BN_D048(band)           MT7996_PHY_BN(band, 0xd048u)	/* table A */
#define MT7996_PHY_BN_D04C(band)           MT7996_PHY_BN(band, 0xd04cu)	/* table A */
#define MT7996_PHY_BN_D050(band)           MT7996_PHY_BN(band, 0xd050u)	/* table A */
#define MT7996_PHY_BN_D054(band)           MT7996_PHY_BN(band, 0xd054u)	/* table A */
#define MT7996_PHY_BN_D058(band)           MT7996_PHY_BN(band, 0xd058u)	/* table A */
#define MT7996_PHY_BN_D05C(band)           MT7996_PHY_BN(band, 0xd05cu)	/* table A */
#define MT7996_PHY_BN_D060(band)           MT7996_PHY_BN(band, 0xd060u)	/* table A */
#define MT7996_PHY_BN_D064(band)           MT7996_PHY_BN(band, 0xd064u)	/* table A */
#define MT7996_PHY_BN_D068(band)           MT7996_PHY_BN(band, 0xd068u)	/* table A */
#define MT7996_PHY_BN_D0A0(band)           MT7996_PHY_BN(band, 0xd0a0u)	/* table A */
#define MT7996_PHY_BN_D0B4(band)           MT7996_PHY_BN(band, 0xd0b4u)	/* table A */
#define MT7996_PHY_BN_D0B8(band)           MT7996_PHY_BN(band, 0xd0b8u)	/* table A */
#define MT7996_PHY_BN_D0BC(band)           MT7996_PHY_BN(band, 0xd0bcu)	/* table A */

/* ========================================================================
 * PHYDFE_CTRL_BN: 0x8308e000 + band<<20 .. 0x8308ffff (ROM modules 0x72
 * band 0 / 0x73 band 1 CMM, 0x6d PWR_REGU; no ROM table for band 2).
 * TX precompensation (+0xe204..), the per-band PHY init's TX words
 * (+0xe2a4..+0xe334), TPC (+0xe400..+0xe7d0), the band tone
 * generator and TSSI chain select (+0xfc00, +0xfc04, +0xfc0c), TSSI core
 * (+0xfd00..+0xfe60, with DPD's DFE gain compensation +0xfdf0..+0xfe0c) and
 * the DPD LUT select (+0xff00, +0xff44): phy_cal.h. DSP-side words
 * +0xef04..+0xf088 (DSP trigger enable/pulse at +0xef08/+0xef10): dsp.h.
 * ======================================================================== */

/* +0xf000: [3:1] cleared by the channel-set D-die RX setup on every band
 * (V:e00de084), then = 5 at BW160 (V:e00de4b4), keyed on the BW class, not
 * the band: the stock band-1 set is the BW160 one (0x0107d630 -> 0x0107d63a;
 * bands 0, 2 unchanged). [static][measured] */
#define MT7996_PHY_DFE_F000(band)         MT7996_PHY_BN(band, 0xf000u)
#define   MT7996_PHY_DFE_F000_F3_1        HW_GENMASK(3, 1)
#define     MT7996_PHY_DFE_F000_F3_1_BW160 5u
/* +0xf008: [4] set by the vendor ICAP manual-gain recipe (cleared on
 * restore); a gain calibration writes old[12:0] | 0x200000 | 0x10. [static] */
#define MT7996_PHY_DFE_F008(band)         MT7996_PHY_BN(band, 0xf008u)
#define   MT7996_PHY_DFE_F008_B4          HW_BIT(4)
#define   MT7996_PHY_DFE_F008_B21         HW_BIT(21)
/* +0xf100: [0] cleared by T1[23] (mode 0x16, phy_cal.h; observed 0x1e1 ->
 * 0x1e0); the per-band PHY init's register table writes 0x1e1. Not
 * decoded. [static][measured] */
#define MT7996_PHY_DFE_F100(band)         MT7996_PHY_BN(band, 0xf100u)
#define   MT7996_PHY_DFE_F100_B0          HW_BIT(0)
/* +0xf168: read-modified by every channel set right after
 * MT7996_PHY_BN_6188 (V:e00d188e): 0x8b007dc0 -> 0x8b010c5f / 0x8b007cd5
 * / 0x8b006b59 at BW20 / BW160 / BW320. The low 17 bits written equal
 * MT7996_PHY_CHAIN_D0AC's value >> 0 / 3 / 4 at the same bandwidths
 * [inferred: one quantity behind both]. In the receive-only subset; field
 * bounds not decoded. [static][measured] */
#define MT7996_PHY_DFE_F168(band)         MT7996_PHY_BN(band, 0xf168u)
#define   MT7996_PHY_DFE_F168_VAL         HW_GENMASK(17, 0)	/* round(0xa000000 / f_MHz), the channel set */
/* +0xf028 [31:30] (band 1 only) and +0xf088 [6:4]: SW-LNA profile fields.
 * The per-band PHY init's register table writes +0xf088 = 0x03885e00; it
 * writes 27 words in +0xf088..+0xf240 in all (with +0xf100 and RXFE_CTRL;
 * phy_map.h MT7996_PHY_REGTBL_*). [static][measured] */
#define MT7996_PHY_DFE_F028(band)         MT7996_PHY_BN(band, 0xf028u)
#define MT7996_PHY_DFE_F088(band)         MT7996_PHY_BN(band, 0xf088u)

/*
 * RXFE_CTRL, +0xf110, RW (no ROM name). [static]
 *  [7:4]  code written with BN_MODE[27:24] by the WM PHY selector programmer
 *         (codes 0..4); a calibration prefix first writes 4.
 *  [1:0]  set to 3 by the (default-disabled) D-die AFE/RXFE calibration,
 *         which also writes [7:4] = 4.
 *  [19]   cleared by the TTG-tone RX calibrations (phy_cal.h T1{18..23}).
 * Meaning of the codes not decoded. DPD's TX power sweep (phy_cal.h
 * T2{0..3}) writes [7:4] with BN_MODE[27:24] per bandwidth step and
 * restores only BN_MODE, so [7:4] is left at its last code (1 / 3 / 4 on
 * bands 0 / 1 / 2) while BN_MODE[27:24] is restored to 0 / 3 / 4: the two
 * disagree on band 0. T2{6} then writes the same [7:4] = 1, so MediaTek's
 * receive path runs band 0 with [7:4] = 1; a firmware without these items
 * keeps the channel set's 0 (not a known fault). [static, measured]
 * The per-band PHY init's register table writes 0x01010000 at calibration
 * init ([7:4] = 0, [1:0] = 0; [24] and [16] set, not decoded), before the
 * channel sets and the calibration items rewrite [7:4]. [static][measured]
 */
#define MT7996_PHY_RXFE_CTRL(band)        MT7996_PHY_BN(band, 0xf110u)
#define   MT7996_PHY_RXFE_CTRL_B19        HW_BIT(19)
#define   MT7996_PHY_RXFE_CTRL_CODE       HW_GENMASK(7, 4)
#define   MT7996_PHY_RXFE_CTRL_1_0        HW_GENMASK(1, 0)
#define   MT7996_PHY_RXFE_CTRL_INIT       0x01010000u

/*
 * WF_RXFE_01, +0xf114, RW: digital RX front end (ADC -> async FIFO ->
 * manual downsample -> DLPF). [static: ROM names, all prefixed BAND_]
 * Live value 0x08000000 in normal RX on bands 0 and 1 (only RXFDIQ_TAP_NUM
 * = 2) [measured]. Calibration writes RXFDIQ_TAP_NUM = 2 here and at the
 * twin +0x10000 (0x8309f114 + band<<20); no WM code writes any other field
 * (ROM TOAE helpers write [2] and [7], below; T1[23] rewrites the word with
 * [19] DBG_SEL_RX = 0, already 0). Writing each of the 13 fields on band 1
 * produced no structural change of the captured samples, and AFIFO_RD_EN =
 * 0 while the receiver decodes, so [2] is not the datapath FIFO enable.
 * [measured]
 * [3] is listed twice in the ROM table (inside DLPF_MODE_MAN_CTL and as
 * AFIFO_CK320M); the setters overwrite each other in call order.
 * ROM halToaeSetAFIFO sets [2]; ROM halToaeDownSampleOn sets [7] (the
 * filter-reset disable), not MAN_DOWNSAMP. Legal values of MAN_DOWNSAMP and
 * DLPF_MODE_MAN_CTL are not known. On band 1 at the 160 MHz class (320 Msps,
 * MT7977 filter at 320 MHz) DLPF_MODE_MAN_EN with CTL 0..7 leaves the
 * captured response within about 1 dB to +-170 MHz (dfe.h "Taps"). [measured]
 */
#define MT7996_PHY_RXFE01(band)           MT7996_PHY_BN(band, 0xf114u)
#define   MT7996_PHY_RXFE01_ADCDC_COMP_VAL HW_GENMASK(31, 28)
#define   MT7996_PHY_RXFE01_RXFDIQ_TAP_NUM HW_GENMASK(27, 26)
#define   MT7996_PHY_RXFE01_DBG_SEL_RX    HW_BIT(19)
#define   MT7996_PHY_RXFE01_DLPF_BW8080C_ACI_SEL HW_BIT(18)
#define   MT7996_PHY_RXFE01_ADC480M_MODE  HW_BIT(16)
#define   MT7996_PHY_RXFE01_WB_SEL        HW_GENMASK(15, 12)
#define   MT7996_PHY_RXFE01_TSSI_BW       HW_GENMASK(11, 8)
#define   MT7996_PHY_RXFE01_DIS_ADC_FLT_RST HW_BIT(7)
#define   MT7996_PHY_RXFE01_DLPF_MODE_MAN_EN HW_BIT(6)
#define   MT7996_PHY_RXFE01_DLPF_MODE_MAN_CTL HW_GENMASK(5, 3)
#define   MT7996_PHY_RXFE01_AFIFO_CK320M  HW_BIT(3)
#define   MT7996_PHY_RXFE01_AFIFO_RD_EN   HW_BIT(2)
#define   MT7996_PHY_RXFE01_MAN_DOWNSAMP  HW_GENMASK(1, 0)
#define   MT7996_PHY_RXFE01_LIVE          0x08000000u	/* normal RX [measured] */
/* The twin written alongside it (no ROM table entry). [static] */
#define MT7996_PHY_RXFE01_TWIN(band)      MT7996_PHY_BAND_REG(band, 0x9f114u)

/*
 * RXFE_MAN, +0xf118, RW (no ROM name): [24] enable + [21:16] 6-bit value.
 * The vendor ICAP manual-gain recipe sets [24] and clears [21:16]; its
 * restore clears only [24], so [21:16] stays 0. A gain calibration sets [24]
 * with [21:16] = its argument.
 * [static; "manual 6-bit override" inferred]
 */
#define MT7996_PHY_RXFE_MAN(band)         MT7996_PHY_BN(band, 0xf118u)
#define   MT7996_PHY_RXFE_MAN_EN          HW_BIT(24)
#define   MT7996_PHY_RXFE_MAN_VAL         HW_GENMASK(21, 16)

/* +0xfc20: WM PHY init writes 0; the WM PHY register helper sets [0] unless
 * mode 16. [static] */
#define MT7996_PHY_DFE_FC20(band)         MT7996_PHY_BN(band, 0xfc20u)
#define   MT7996_PHY_DFE_FC20_B0          HW_BIT(0)
/* +0xfcb0 / +0xfcb4: WM PHY preparation writes the same pattern word to both,
 * chosen by a configuration byte (see phy_map.h); the channel-set D-die RX
 * setup writes them too, by die type (phy_cal.h): 0x00123456 (_CFG6, band
 * 0's MT7976, EPA boards) and 0x00123455 (_CFG7, the MT7977 bands). [static] */
#define MT7996_PHY_DFE_FCB0(band)         MT7996_PHY_BN(band, 0xfcb0u)
#define MT7996_PHY_DFE_FCB4(band)         MT7996_PHY_BN(band, 0xfcb4u)
#define   MT7996_PHY_DFE_FCB_PAT_CFG5     0x01234567u
#define   MT7996_PHY_DFE_FCB_PAT_CFG6     0x00123456u
#define   MT7996_PHY_DFE_FCB_PAT_CFG7     0x00123455u
/* +0xfd08 (band TSSI core block): [31] pulsed (set, then cleared) by the
 * channel-set D-die RX calibration (0x00000002 -> 0x80000002 -> 2); PHY
 * table A (phy_map.h) writes the word whole earlier in the same channel
 * set. Not decoded. [static][measured] */
#define MT7996_PHY_DFE_FD08(band)         MT7996_PHY_BN(band, 0xfd08u)
#define   MT7996_PHY_DFE_FD08_B31         HW_BIT(31)
/* +0xfd34: manual digital TX gain [inferred]. WM PHY init writes 0. The WM's
 * TX gain setter (V:e0049a8c) writes [31] = manual and [23:16] = the gain
 * code (T2{4} tables MT7976 0xe007158c, MT7977 0xe00719fc; T2{5} 0xfc, then
 * table 0xe0080090); T2{6} passes 10-bit codes 0x390..0x3f0 unmasked, and
 * [25:24] read back 0 [measured]. In modes 2 / 0x1b the setter writes TSSI
 * PWR_CTRL [2] (phy_cal.h) and +0xfd38 instead. The TX LO / IQ calibration
 * (phy_cal.h T1{9,13,17}) writes [23:16] = its table code + 8 x the anchor
 * offset, the shift not masked: a negative value also sets [31:24]
 * (observed 0xfff80000, 0xffc80000). [static][measured] */
#define MT7996_PHY_DFE_FD34(band)         MT7996_PHY_BN(band, 0xfd34u)
#define   MT7996_PHY_DFE_FD34_MAN         HW_BIT(31)
#define   MT7996_PHY_DFE_FD34_GAIN        HW_GENMASK(23, 16)
/* +0xfd38: [12] manual DFE TX gain enable, [9:0] DFE TX gain, signed (1/8
 * dB [inferred]): the TX gain setter in modes 2 / 0x1b (V:e0049a8c), T2{8}
 * (OTFIQ CFG2 FD38_EN / FD38) and DPD (start 0x3d0 = -48, the sweep 0x13d0;
 * its range rule and the LUT gain words that read it: phy_cal.h). [static] */
#define MT7996_PHY_DFE_FD38(band)         MT7996_PHY_BN(band, 0xfd38u)
#define   MT7996_PHY_DFE_FD38_MAN         HW_BIT(12)
#define   MT7996_PHY_DFE_FD38_VAL         HW_GENMASK(9, 0)

/*
 * Twin band block 0x83090000 + band<<20 (no ROM table). The WM writes the
 * same offsets here as in 0x83080000 in the same sequences: +0x0000[12]
 * cleared, +0x0004/+0x0008 = 0x01b60000, +0x0014 reset pulse, +0xf114
 * RXFDIQ_TAP_NUM = 2. Role (second datapath instance) [inferred].
 */
#define MT7996_PHY_BN2(band, off)         MT7996_PHY_BAND_REG(band, 0x90000u + (off))

/* Offset names, role unknown unless a header says more: rows of the per-band PHY init's
 * register tables (phy_init.h MT7996_PHY_INIT_REGTBL_*; "init", written whole on every
 * band, never read by the init) and of the channel-set PHY tables A / B (phy_chan.h
 * MT7996_CHSET_TBL_A_ROWS / _B_ROWS; "table A" / "table B"), and words the channel set
 * writes by field. [static][measured]
 * Sub-blocks: +0xf090..+0xf19c DFE_BN_RXFE (digital RX front end), +0xf200..+0xf240
 * (after RXFE; no map entry). */
#define MT7996_PHY_DFE_E200(band)          MT7996_PHY_BN(band, 0xe200u)	/* table A */
#define MT7996_PHY_DFE_E428(band)          MT7996_PHY_BN(band, 0xe428u)	/* table A */
#define MT7996_PHY_DFE_E430(band)          MT7996_PHY_BN(band, 0xe430u)	/* table A */

/* +0xe738 (TPC_MCS_LG, of the ROM's TPC_MCS_{LG,HT,VHT,HE} +0xe738..+0xe758, 3-bit
 * per-MCS classes, phy_cal.h): with E2 parts and EPA, table A's explicit fields
 * write [18:16] = 3, then 3 in the four 3-bit fields [2:0], [6:4], [10:8], [14:12]
 * of +0xe738, +0xe73c, +0xe74c (MediaTek's loop index arithmetic: offsets 0, 4,
 * 0x14), then of +0xe750 and +0xe758 (phy_cal.h "Channel set"). [static] */
#define MT7996_PHY_DFE_E738(band)          MT7996_PHY_BN(band, 0xe738u)	/* table A */
#define   MT7996_PHY_DFE_E738_F18_16    HW_GENMASK(18, 16)
#define   MT7996_PHY_DFE_E738_F2_0      HW_GENMASK(2, 0)	/* also +0xe73c, +0xe74c, +0xe750, +0xe758 */
#define   MT7996_PHY_DFE_E738_F6_4      HW_GENMASK(6, 4)
#define   MT7996_PHY_DFE_E738_F10_8     HW_GENMASK(10, 8)
#define   MT7996_PHY_DFE_E738_F14_12    HW_GENMASK(14, 12)

#define MT7996_PHY_DFE_E73C(band)          MT7996_PHY_BN(band, 0xe73cu)	/* table A */
#define MT7996_PHY_DFE_E740(band)          MT7996_PHY_BN(band, 0xe740u)	/* table A */
#define MT7996_PHY_DFE_E744(band)          MT7996_PHY_BN(band, 0xe744u)	/* table A */
#define MT7996_PHY_DFE_E748(band)          MT7996_PHY_BN(band, 0xe748u)	/* table A */
#define MT7996_PHY_DFE_E74C(band)          MT7996_PHY_BN(band, 0xe74cu)	/* table A */
#define MT7996_PHY_DFE_E750(band)          MT7996_PHY_BN(band, 0xe750u)	/* table A */
#define MT7996_PHY_DFE_E754(band)          MT7996_PHY_BN(band, 0xe754u)	/* table A */
#define MT7996_PHY_DFE_E758(band)          MT7996_PHY_BN(band, 0xe758u)	/* table A */
#define MT7996_PHY_DFE_E75C(band)          MT7996_PHY_BN(band, 0xe75cu)	/* table A */
#define MT7996_PHY_DFE_E760(band)          MT7996_PHY_BN(band, 0xe760u)	/* table A */
#define MT7996_PHY_DFE_E764(band)          MT7996_PHY_BN(band, 0xe764u)	/* table A */
#define MT7996_PHY_DFE_F020(band)          MT7996_PHY_BN(band, 0xf020u)	/* table B */
#define MT7996_PHY_DFE_F024(band)          MT7996_PHY_BN(band, 0xf024u)	/* table B */
#define MT7996_PHY_DFE_F02C(band)          MT7996_PHY_BN(band, 0xf02cu)	/* table B */
#define MT7996_PHY_DFE_F030(band)          MT7996_PHY_BN(band, 0xf030u)	/* table B */
#define MT7996_PHY_DFE_F090(band)          MT7996_PHY_BN(band, 0xf090u)	/* init */
#define MT7996_PHY_DFE_F094(band)          MT7996_PHY_BN(band, 0xf094u)	/* init */
#define MT7996_PHY_DFE_F130(band)          MT7996_PHY_BN(band, 0xf130u)	/* table B */
#define MT7996_PHY_DFE_F160(band)          MT7996_PHY_BN(band, 0xf160u)	/* table B */
#define MT7996_PHY_DFE_F16C(band)          MT7996_PHY_BN(band, 0xf16cu)	/* init */
#define MT7996_PHY_DFE_F170(band)          MT7996_PHY_BN(band, 0xf170u)	/* table B */
#define MT7996_PHY_DFE_F174(band)          MT7996_PHY_BN(band, 0xf174u)	/* init */
#define MT7996_PHY_DFE_F194(band)          MT7996_PHY_BN(band, 0xf194u)	/* init */
#define MT7996_PHY_DFE_F198(band)          MT7996_PHY_BN(band, 0xf198u)	/* init */
#define MT7996_PHY_DFE_F19C(band)          MT7996_PHY_BN(band, 0xf19cu)	/* init */
#define MT7996_PHY_DFE_F200(band)          MT7996_PHY_BN(band, 0xf200u)	/* init */
#define MT7996_PHY_DFE_F204(band)          MT7996_PHY_BN(band, 0xf204u)	/* init */
#define MT7996_PHY_DFE_F208(band)          MT7996_PHY_BN(band, 0xf208u)	/* init */
#define MT7996_PHY_DFE_F20C(band)          MT7996_PHY_BN(band, 0xf20cu)	/* init */
#define MT7996_PHY_DFE_F210(band)          MT7996_PHY_BN(band, 0xf210u)	/* init */
#define MT7996_PHY_DFE_F214(band)          MT7996_PHY_BN(band, 0xf214u)	/* init */
#define MT7996_PHY_DFE_F218(band)          MT7996_PHY_BN(band, 0xf218u)	/* init */
#define MT7996_PHY_DFE_F21C(band)          MT7996_PHY_BN(band, 0xf21cu)	/* init */
#define MT7996_PHY_DFE_F220(band)          MT7996_PHY_BN(band, 0xf220u)	/* init */
#define MT7996_PHY_DFE_F224(band)          MT7996_PHY_BN(band, 0xf224u)	/* init */
#define MT7996_PHY_DFE_F228(band)          MT7996_PHY_BN(band, 0xf228u)	/* init */
#define MT7996_PHY_DFE_F22C(band)          MT7996_PHY_BN(band, 0xf22cu)	/* init */
#define MT7996_PHY_DFE_F230(band)          MT7996_PHY_BN(band, 0xf230u)	/* init */
#define MT7996_PHY_DFE_F234(band)          MT7996_PHY_BN(band, 0xf234u)	/* init */
#define MT7996_PHY_DFE_F238(band)          MT7996_PHY_BN(band, 0xf238u)	/* init */
#define MT7996_PHY_DFE_F23C(band)          MT7996_PHY_BN(band, 0xf23cu)	/* init */
#define MT7996_PHY_DFE_F240(band)          MT7996_PHY_BN(band, 0xf240u)	/* init */
#define MT7996_PHY_DFE_FD14(band)          MT7996_PHY_BN(band, 0xfd14u)	/* table A */
#define MT7996_PHY_DFE_FD30(band)          MT7996_PHY_BN(band, 0xfd30u)	/* table A */
#define MT7996_PHY_DFE_FD70(band)          MT7996_PHY_BN(band, 0xfd70u)	/* table A */
#define MT7996_PHY_DFE_FD74(band)          MT7996_PHY_BN(band, 0xfd74u)	/* table A */
#define MT7996_PHY_DFE_FD78(band)          MT7996_PHY_BN(band, 0xfd78u)	/* table A */
#define MT7996_PHY_DFE_FD7C(band)          MT7996_PHY_BN(band, 0xfd7cu)	/* table A */
#define MT7996_PHY_DFE_FD80(band)          MT7996_PHY_BN(band, 0xfd80u)	/* table A */
#define MT7996_PHY_DFE_FD84(band)          MT7996_PHY_BN(band, 0xfd84u)	/* table A */
#define MT7996_PHY_DFE_FD90(band)          MT7996_PHY_BN(band, 0xfd90u)	/* table A */
#define MT7996_PHY_DFE_FD94(band)          MT7996_PHY_BN(band, 0xfd94u)	/* table A */
#define MT7996_PHY_DFE_FD9C(band)          MT7996_PHY_BN(band, 0xfd9cu)	/* table A */
#define MT7996_PHY_DFE_FDA0(band)          MT7996_PHY_BN(band, 0xfda0u)	/* table A */
#define MT7996_PHY_DFE_FDA4(band)          MT7996_PHY_BN(band, 0xfda4u)	/* table A */
#define MT7996_PHY_DFE_FDB0(band)          MT7996_PHY_BN(band, 0xfdb0u)	/* table A */
#define MT7996_PHY_DFE_FDB4(band)          MT7996_PHY_BN(band, 0xfdb4u)	/* table A */
#define MT7996_PHY_DFE_FDBC(band)          MT7996_PHY_BN(band, 0xfdbcu)	/* table A */
#define MT7996_PHY_DFE_FDC0(band)          MT7996_PHY_BN(band, 0xfdc0u)	/* table A */
#define MT7996_PHY_DFE_FDC4(band)          MT7996_PHY_BN(band, 0xfdc4u)	/* table A */
#define MT7996_PHY_DFE_FDD0(band)          MT7996_PHY_BN(band, 0xfdd0u)	/* table A */
#define MT7996_PHY_DFE_FDD4(band)          MT7996_PHY_BN(band, 0xfdd4u)	/* table A */
#define MT7996_PHY_DFE_FDDC(band)          MT7996_PHY_BN(band, 0xfddcu)	/* table A */
#define MT7996_PHY_DFE_FDE0(band)          MT7996_PHY_BN(band, 0xfde0u)	/* table A */
#define MT7996_PHY_DFE_FDE8(band)          MT7996_PHY_BN(band, 0xfde8u)	/* table A */
#define MT7996_PHY_DFE_FDEC(band)          MT7996_PHY_BN(band, 0xfdecu)	/* table A */
#define MT7996_PHY_DFE_FE10(band)          MT7996_PHY_BN(band, 0xfe10u)	/* table A */
#define MT7996_PHY_DFE_FE14(band)          MT7996_PHY_BN(band, 0xfe14u)	/* table A */
#define MT7996_PHY_DFE_FE18(band)          MT7996_PHY_BN(band, 0xfe18u)	/* table A */
#define MT7996_PHY_DFE_FE1C(band)          MT7996_PHY_BN(band, 0xfe1cu)	/* table A */
#define MT7996_PHY_DFE_FE20(band)          MT7996_PHY_BN(band, 0xfe20u)	/* table A */
#define MT7996_PHY_DFE_FE24(band)          MT7996_PHY_BN(band, 0xfe24u)	/* table A */
#define MT7996_PHY_DFE_FE28(band)          MT7996_PHY_BN(band, 0xfe28u)	/* table A */
#define MT7996_PHY_DFE_FE2C(band)          MT7996_PHY_BN(band, 0xfe2cu)	/* table A */
#define MT7996_PHY_DFE_FE40(band)          MT7996_PHY_BN(band, 0xfe40u)	/* table A */
#define MT7996_PHY_DFE_FE44(band)          MT7996_PHY_BN(band, 0xfe44u)	/* table A */
#define MT7996_PHY_DFE_FE48(band)          MT7996_PHY_BN(band, 0xfe48u)	/* table A */
#define MT7996_PHY_DFE_FE4C(band)          MT7996_PHY_BN(band, 0xfe4cu)	/* table A */
#define MT7996_PHY_DFE_FE64(band)          MT7996_PHY_BN(band, 0xfe64u)	/* table A */
#define MT7996_PHY_DFE_FF04(band)          MT7996_PHY_BN(band, 0xff04u)	/* table A */

/* +0xff14 / +0xff20 / +0xff2c [23:0], +0xff18 / +0xff24 [31:8], +0xff30 [31:24],
 * +0xff80 [15:8] / [7:0]: table A's explicit fields with E2 parts, EPA and the
 * DPD table not forced (V:e00d92c2..; phy_chan.h MT7996_CHSET_FF*_VAL). Role
 * unknown. [static][measured] */
#define MT7996_PHY_DFE_FF14(band)          MT7996_PHY_BN(band, 0xff14u)	/* table A */
#define   MT7996_PHY_DFE_FF14_LO24      HW_GENMASK(23, 0)	/* also +0xff20, +0xff2c */

#define MT7996_PHY_DFE_FF18(band)          MT7996_PHY_BN(band, 0xff18u)	/* table A */
#define   MT7996_PHY_DFE_FF18_HI24      HW_GENMASK(31, 8)	/* also +0xff24 */
#define MT7996_PHY_DFE_FF1C(band)          MT7996_PHY_BN(band, 0xff1cu)	/* table A */
#define MT7996_PHY_DFE_FF20(band)          MT7996_PHY_BN(band, 0xff20u)	/* table A */
#define MT7996_PHY_DFE_FF24(band)          MT7996_PHY_BN(band, 0xff24u)	/* table A */
#define MT7996_PHY_DFE_FF28(band)          MT7996_PHY_BN(band, 0xff28u)	/* table A */
#define MT7996_PHY_DFE_FF2C(band)          MT7996_PHY_BN(band, 0xff2cu)	/* table A */
#define MT7996_PHY_DFE_FF30(band)          MT7996_PHY_BN(band, 0xff30u)	/* table A */
#define   MT7996_PHY_DFE_FF30_HI8       HW_GENMASK(31, 24)
#define MT7996_PHY_DFE_FF34(band)          MT7996_PHY_BN(band, 0xff34u)	/* table A */
#define MT7996_PHY_DFE_FF38(band)          MT7996_PHY_BN(band, 0xff38u)	/* table A */
#define MT7996_PHY_DFE_FF3C(band)          MT7996_PHY_BN(band, 0xff3cu)	/* table A */
#define MT7996_PHY_DFE_FF40(band)          MT7996_PHY_BN(band, 0xff40u)	/* table A */
#define MT7996_PHY_DFE_FF48(band)          MT7996_PHY_BN(band, 0xff48u)	/* table A */
#define MT7996_PHY_DFE_FF4C(band)          MT7996_PHY_BN(band, 0xff4cu)	/* table A */
#define MT7996_PHY_DFE_FF50(band)          MT7996_PHY_BN(band, 0xff50u)	/* table A */
#define MT7996_PHY_DFE_FF54(band)          MT7996_PHY_BN(band, 0xff54u)	/* table A */
#define MT7996_PHY_DFE_FF58(band)          MT7996_PHY_BN(band, 0xff58u)	/* table A */
#define MT7996_PHY_DFE_FF5C(band)          MT7996_PHY_BN(band, 0xff5cu)	/* table A */
#define MT7996_PHY_DFE_FF60(band)          MT7996_PHY_BN(band, 0xff60u)	/* table A */
#define MT7996_PHY_DFE_FF64(band)          MT7996_PHY_BN(band, 0xff64u)	/* table A */

/* +0xff68: table A's explicit fields clear [22:20], [17:16], [28], [27:24],
 * [29], [30], [31] at every channel set, one read-modify-write each, in that
 * order. Role unknown. [static][measured] */
#define MT7996_PHY_DFE_FF68(band)          MT7996_PHY_BN(band, 0xff68u)
#define   MT7996_PHY_DFE_FF68_F22_20    HW_GENMASK(22, 20)
#define   MT7996_PHY_DFE_FF68_F17_16    HW_GENMASK(17, 16)
#define   MT7996_PHY_DFE_FF68_B28       HW_BIT(28)
#define   MT7996_PHY_DFE_FF68_F27_24    HW_GENMASK(27, 24)
#define   MT7996_PHY_DFE_FF68_B29       HW_BIT(29)
#define   MT7996_PHY_DFE_FF68_B30       HW_BIT(30)
#define   MT7996_PHY_DFE_FF68_B31       HW_BIT(31)

#define MT7996_PHY_DFE_FF6C(band)          MT7996_PHY_BN(band, 0xff6cu)	/* table A */
#define MT7996_PHY_DFE_FF74(band)          MT7996_PHY_BN(band, 0xff74u)	/* table A */
#define MT7996_PHY_DFE_FF80(band)          MT7996_PHY_BN(band, 0xff80u)	/* table A */
#define   MT7996_PHY_DFE_FF80_B1        HW_GENMASK(15, 8)
#define   MT7996_PHY_DFE_FF80_B0        HW_GENMASK(7, 0)
#define MT7996_PHY_DFE_FF84(band)          MT7996_PHY_BN(band, 0xff84u)	/* table A */
#define MT7996_PHY_DFE_FF8C(band)          MT7996_PHY_BN(band, 0xff8cu)	/* table A */
#define MT7996_PHY_DFE_FF90(band)          MT7996_PHY_BN(band, 0xff90u)	/* table A */
#define MT7996_PHY_DFE_FF94(band)          MT7996_PHY_BN(band, 0xff94u)	/* table A */
#define MT7996_PHY_DFE_FFAC(band)          MT7996_PHY_BN(band, 0xffacu)	/* table A */
#define MT7996_PHY_DFE_FFB0(band)          MT7996_PHY_BN(band, 0xffb0u)	/* table A */
#define MT7996_PHY_DFE_FFB4(band)          MT7996_PHY_BN(band, 0xffb4u)	/* table A */
#define MT7996_PHY_DFE_FFB8(band)          MT7996_PHY_BN(band, 0xffb8u)	/* table A */
#define MT7996_PHY_DFE_FFBC(band)          MT7996_PHY_BN(band, 0xffbcu)	/* table A */
#define MT7996_PHY_DFE_FFC0(band)          MT7996_PHY_BN(band, 0xffc0u)	/* table A */

/* ========================================================================
 * WF_COMM block: 0x830a0000 + band<<20 (ROM modules 0x53 RDD, 0x55 CSI,
 * 0x57 CMM, 0x59 TOAE, WFRDM 0x36/0x37). Per band: the WM initialises
 * 0x830a.., 0x831a.., 0x832a.. separately. [static]
 *   +0x0000..: DSP-visible words (+0x02b4/+0x02b8 DSP SP/PC mirrors): dsp.h.
 *   +0x1000..+0x10ff: capture trigger block (below).
 *   +0xd000..+0xdfff: DFE capture mux / lane mux / calibration capture
 *              engine (dfe.h); WM<->DSP interface; the TTG RX DC and IQ
 *              engines (+0xd49c..+0xd9c0); the TX LO / IQ calibration
 *              engine (+0xd800..+0xd81c, phy_cal.h); other words below.
 *   +0xe000..+0xe0f0: DPD training engine (phy_cal.h).
 * ======================================================================== */

#define MT7996_PHY_WFCOMM(band, off)      MT7996_PHY_BAND_REG(band, 0xa0000u + (off))

/* WF_COMM +0x0080: [7:4] = the 2.4 GHz primary channel, written by the
 * channel set on 2.4 GHz only (V:e00d1b72; phy_chan.h MT7996_CHSET_0080_*:
 * at BW20 the channel, at BW40 the centre - 2 / + 2 for centres 3..11, else
 * 6; stock band 0 0x00810061 -> 0x00810071, channel 6 -> 7); bands 1 and 2
 * do not write it there. In the receive-only subset. The per-band PHY init's
 * register table writes 0x00810061; PHY table B (phy_map.h) writes WF_COMM
 * +0x0000 / +0x0004 / +0x0050 of the same page. Other bits not decoded.
 * [static][measured] */
#define MT7996_PHY_WFCOMM_0080(band)      MT7996_PHY_WFCOMM(band, 0x0080u)
#define   MT7996_PHY_WFCOMM_0080_CH       HW_GENMASK(7, 4)

/*
 * Capture trigger block, WF_COMM +0x1000 (no ROM name). A second, PHY-side
 * block the firmware capture arm writes besides ICAP (the vendor TM WM's
 * SPECTRUM arm, capture_setup.h). [static, measured]
 * CAPTRIG +0x1000: the capture trigger V:e012e540(event, band, phy) arms it
 *   with five read-modify-writes, in order: [19] set when the trigger event
 *   != MT7996_PHY_CAPTRIG_EVENT_NONE (cleared when it is NONE); [17] cleared
 *   (mask 0xfffdffff); [16] set; [26] set; [15:8] = 1 (or [8] OR-set when the
 *   event is NONE). Reads 0 before and 0x04090100 ([26], [19], [16], [8])
 *   after a firmware arm with event 0. [static][measured]
 * CAPTRIG_CFG +0x1004: [7:0] cleared by the capture clear, = 0x10 at the
 *   capture configure (reads 0x10 after an arm; role not decoded); [9:8]
 *   cleared by the trigger after CAPTRIG. [static, measured]
 * +0x1008: the request's trigger-event word (SPECTRUM word 2), written whole
 *   by the trigger last, unless it is NONE. Role not decoded (the prime
 *   sends 0). [static]
 * +0x1094: free-running counter. +0x1100 reads 0. [measured]
 */
#define MT7996_PHY_CAPTRIG(band)          MT7996_PHY_WFCOMM(band, 0x1000u)
#define   MT7996_PHY_CAPTRIG_B26          HW_BIT(26)
#define   MT7996_PHY_CAPTRIG_B19          HW_BIT(19)
#define   MT7996_PHY_CAPTRIG_B17          HW_BIT(17)
#define   MT7996_PHY_CAPTRIG_B16          HW_BIT(16)
#define   MT7996_PHY_CAPTRIG_F15_8        HW_GENMASK(15, 8)
#define     MT7996_PHY_CAPTRIG_F15_8_START 1u
#define   MT7996_PHY_CAPTRIG_ARMED        0x04090100u	/* read after a firmware arm, event 0 */
#define MT7996_PHY_CAPTRIG_CFG(band)      MT7996_PHY_WFCOMM(band, 0x1004u)
#define   MT7996_PHY_CAPTRIG_CFG_SEL      HW_GENMASK(7, 0)
#define     MT7996_PHY_CAPTRIG_CFG_SEL_CAPTURE 0x10u
#define   MT7996_PHY_CAPTRIG_CFG_F9_8     HW_GENMASK(9, 8)
#define MT7996_PHY_CAPTRIG_EVENT(band)    MT7996_PHY_WFCOMM(band, 0x1008u)
#define   MT7996_PHY_CAPTRIG_EVENT_NONE   0xffffffffu
#define MT7996_PHY_CAPTRIG_COUNTER(band)  MT7996_PHY_WFCOMM(band, 0x1094u)

/*
 * Radar detectors RDD0..2. [static: ROM names]
 * RDD_TOP_5 +0x2014 / RDD1_TOP_5 +0x2114 / RDD2_TOP_5 +0x2164:
 *   [31] TEST_PULSE_EN, [30] TEST_DET_PULSE (hardware radar test-pulse
 *   generator), [28] PULSE_BUF_CLR, [27:24] PULSE_BUF_SEL, [23] WAKEUP_IRQ_EN,
 *   [22] ERROR_IRQ_EN, [20:16] NO_DET_PULSE_PERIOD, [12:8] WAKE_UP_PULSE_NUM,
 *   [7:0] WAKE_UP_PERIOD.
 * RDD_TOP_12 +0x2030 / RDD1_TOP_12 +0x2130: [30:28] ANT_SEL (chain feeding
 *   the detector), [27] BAND_SEL, [26:25] CCA_SEL, [14] MDRDY_SEL,
 *   [6:0] MDRDY_START. +0x2130 is named RDD1_TOP_12 in one ROM table and
 *   RDD2_TOP_12 in another; the RDD2 start writes its ANT_SEL = 4 (the
 *   monitor chain).
 * +0x2148 [20]: cleared by the RDD2 start.
 * Radar pulse records leave through the AHB bridge (pulse descriptors
 * {start, width, power, ...}, not IQ).
 */
#define MT7996_PHY_RDD_TOP5(band, rdd) \
	MT7996_PHY_WFCOMM(band, (rdd) == 0 ? 0x2014u : (rdd) == 1 ? 0x2114u : 0x2164u)
#define   MT7996_PHY_RDD_TOP5_TEST_PULSE_EN HW_BIT(31)
#define   MT7996_PHY_RDD_TOP5_TEST_DET_PULSE HW_BIT(30)
#define   MT7996_PHY_RDD_TOP5_PULSE_BUF_CLR HW_BIT(28)
#define   MT7996_PHY_RDD_TOP5_PULSE_BUF_SEL HW_GENMASK(27, 24)
#define   MT7996_PHY_RDD_TOP5_WAKEUP_IRQ_EN HW_BIT(23)
#define   MT7996_PHY_RDD_TOP5_ERROR_IRQ_EN HW_BIT(22)
#define   MT7996_PHY_RDD_TOP5_NO_DET_PERIOD HW_GENMASK(20, 16)
#define   MT7996_PHY_RDD_TOP5_WAKE_PULSE_NUM HW_GENMASK(12, 8)
#define   MT7996_PHY_RDD_TOP5_WAKE_PERIOD HW_GENMASK(7, 0)
#define MT7996_PHY_RDD_TOP12(band, rdd)   MT7996_PHY_WFCOMM(band, (rdd) ? 0x2130u : 0x2030u)
#define   MT7996_PHY_RDD_TOP12_ANT_SEL    HW_GENMASK(30, 28)
#define   MT7996_PHY_RDD_TOP12_BAND_SEL   HW_BIT(27)
#define   MT7996_PHY_RDD_TOP12_CCA_SEL    HW_GENMASK(26, 25)
#define   MT7996_PHY_RDD_TOP12_MDRDY_SEL  HW_BIT(14)
#define   MT7996_PHY_RDD_TOP12_MDRDY_START HW_GENMASK(6, 0)
#define MT7996_PHY_RDD_2148(band)         MT7996_PHY_WFCOMM(band, 0x2148u)
#define   MT7996_PHY_RDD_2148_B20         HW_BIT(20)	/* cleared by the RDD start (V:e00cac46, a2 = 0) */

/*
 * WF_COMM +0x3000 control words (no ROM names). [static]
 *  +0x3000: [31] cleared by WM PHY preparation; [3] set by the WM PHY
 *           register helper.
 *  +0x3004: preparation writes [10:8] = 2, [14:12] = 3, clears [27:16].
 *  +0x3008: preparation sets [18], [19], [23], then writes 0; a capture
 *           clear sets [21] then [22]; capture configure clears [22:21];
 *           every calibration end writes 0x007c00ff then 0 (below); [4]
 *           pulsed (0, 1, 0) before the TTG RX IQ table fit (V:e004a11e).
 *  +0x3020: = 0x1010 by the DFE path setup V:e0048b8a (0x1111 with its a1
 *           = 1), in the TTG RX calibrations. [static][measured]
 *  +0x3050 / +0x3054: [29] CSI / AHB-bridge clock (with BN_CLK0/1[29]),
 *           [measured: required for bridge writes]; the RDD2 start writes
 *           0x10 to both.
 *  +0x3078 / +0x307c: [12:0] = 0x1800 for the background receiver.
 * The per-band PHY init (below) writes, every band: [static][measured]
 *  +0x3034 [11:9] = 0 (0xfffffe3f -> 0xfffff03f);
 *  +0x303c [15] = 0, then [12:11] = 0 (0xf83fff3f -> 0xf83f673f);
 *  +0x3040 [31:23] = 0 (0x7f800063 -> 0x63); +0x3044 [2:0] = 0 (0x1f8
 *           unchanged).
 */
#define MT7996_PHY_WFCOMM_3000(band)      MT7996_PHY_WFCOMM(band, 0x3000u)
/* +0x3000 fields of the channel-set D-die RX setup V:e00ddd0e: [31] = 0 (V:e00de02a),
 * [16] = 1 after the gate (V:e00de1b2), and on 2.4 GHz (base 2,407,000 kHz) [31:28] = 0
 * (V:e00de1e6). Stock 0x00212000 throughout. Roles unknown. [static][measured] */
#define   MT7996_PHY_WFCOMM_3000_B31      HW_BIT(31)
#define   MT7996_PHY_WFCOMM_3000_B16      HW_BIT(16)
#define   MT7996_PHY_WFCOMM_3000_F31_28   HW_GENMASK(31, 28)
#define MT7996_PHY_WFCOMM_3004(band)      MT7996_PHY_WFCOMM(band, 0x3004u)
#define MT7996_PHY_WFCOMM_3008(band)      MT7996_PHY_WFCOMM(band, 0x3008u)
#define   MT7996_PHY_WFCOMM_3008_CAP_CLR  HW_GENMASK(22, 21)
#define   MT7996_PHY_WFCOMM_3008_CAP_CLR_B21 HW_BIT(21)	/* the capture clear sets [21], then [22] */
#define   MT7996_PHY_WFCOMM_3008_CAP_CLR_B22 HW_BIT(22)
#define   MT7996_PHY_WFCOMM_3008_FIT      HW_BIT(4)	/* pulsed before the IQ table fit */
#define MT7996_PHY_WFCOMM_3020(band)      MT7996_PHY_WFCOMM(band, 0x3020u)
#define MT7996_PHY_CSI_CLK0(band)         MT7996_PHY_WFCOMM(band, 0x3050u)
#define MT7996_PHY_CSI_CLK1(band)         MT7996_PHY_WFCOMM(band, 0x3054u)
#define   MT7996_PHY_CSI_CLK_EN           HW_BIT(29)
#define MT7996_PHY_BGRX_3078(band)        MT7996_PHY_WFCOMM(band, 0x3078u)
#define MT7996_PHY_BGRX_307C(band)        MT7996_PHY_WFCOMM(band, 0x307cu)
#define   MT7996_PHY_BGRX_307X_VAL        HW_GENMASK(12, 0)	/* 0x1800 when on */
#define MT7996_PHY_WFCOMM_3034(band)      MT7996_PHY_WFCOMM(band, 0x3034u)
#define   MT7996_PHY_WFCOMM_3034_F11_9    HW_GENMASK(11, 9)
#define MT7996_PHY_WFCOMM_303C(band)      MT7996_PHY_WFCOMM(band, 0x303cu)
#define   MT7996_PHY_WFCOMM_303C_B15      HW_BIT(15)
#define   MT7996_PHY_WFCOMM_303C_F12_11   HW_GENMASK(12, 11)
#define MT7996_PHY_WFCOMM_3040(band)      MT7996_PHY_WFCOMM(band, 0x3040u)
#define   MT7996_PHY_WFCOMM_3040_F31_23   HW_GENMASK(31, 23)
#define MT7996_PHY_WFCOMM_3044(band)      MT7996_PHY_WFCOMM(band, 0x3044u)
#define   MT7996_PHY_WFCOMM_3044_F2_0     HW_GENMASK(2, 0)

/* WF_COMM +0x3824..+0x38f0, written by the per-band PHY init (below) on
 * every band; in no map: +0x3824 [15:0] = 0xffff, then [31:24] = 0x3f
 * (0x41a5ffff -> 0x3fa5ffff); +0x3844 [19] = 0 (0x013e0e20 -> 0x01360e20);
 * +0x38b4 [15:0] = 0xa000 (0x2000 -> 0xa000); +0x38c4 [31:16] = 0x8000
 * (0x2000e000 -> 0x8000e000); +0x38e4 = 0x10101010 (write only); +0x38f0
 * [7:0] = 4 (0x00130004 unchanged). Not decoded. [static][measured] */
#define MT7996_PHY_WFCOMM_3824(band)      MT7996_PHY_WFCOMM(band, 0x3824u)
#define   MT7996_PHY_WFCOMM_3824_LO       HW_GENMASK(15, 0)
#define   MT7996_PHY_WFCOMM_3824_HI       HW_GENMASK(31, 24)
#define   MT7996_PHY_WFCOMM_3824_LO_INIT  0xffffu	/* e00deb40 / e00deb48 */
#define   MT7996_PHY_WFCOMM_3824_HI_INIT  0x3fu	/* e00deb54 */
#define MT7996_PHY_WFCOMM_3844(band)      MT7996_PHY_WFCOMM(band, 0x3844u)
#define   MT7996_PHY_WFCOMM_3844_B19      HW_BIT(19)
#define MT7996_PHY_WFCOMM_38B4(band)      MT7996_PHY_WFCOMM(band, 0x38b4u)
#define   MT7996_PHY_WFCOMM_38B4_LO       HW_GENMASK(15, 0)
#define   MT7996_PHY_WFCOMM_38B4_LO_INIT  0xa000u	/* e00deb76 */
#define MT7996_PHY_WFCOMM_38C4(band)      MT7996_PHY_WFCOMM(band, 0x38c4u)
#define   MT7996_PHY_WFCOMM_38C4_HI       HW_GENMASK(31, 16)
#define   MT7996_PHY_WFCOMM_38C4_HI_INIT  0x8000u	/* e00deb00 */
#define MT7996_PHY_WFCOMM_38E4(band)      MT7996_PHY_WFCOMM(band, 0x38e4u)
#define   MT7996_PHY_WFCOMM_38E4_INIT     0x10101010u
#define MT7996_PHY_WFCOMM_38F0(band)      MT7996_PHY_WFCOMM(band, 0x38f0u)
#define   MT7996_PHY_WFCOMM_38F0_LO       HW_GENMASK(7, 0)
#define   MT7996_PHY_WFCOMM_38F0_LO_INIT  4u	/* e00deb36 */

/* Calibration-end PHY pulse (V:e0047e36 MT7977 bodies, V:e0049416 MT7976),
 * after the D-die list restore: WF_COMM +0x3008 = 0x007c00ff then 0;
 * RST_CTRL = 0x0103ffff then 0; 0x8308ef00 (+band << 20, the first word of
 * DFE_BN_DSP, phy_map.h) [2] set then cleared; 10 us. All three are in the
 * D-die backup list, so their final values are the pulse's (0, 0, [2] = 0).
 * The TTG RX calibrations (phy_cal.h T1{18..23}) also run V:e0047e36 as a
 * reset pulse around every DC and IQ engine run; the TX LO / IQ calibration
 * (T1{9,13,17}) runs V:e0049416 before every engine run and power-meter
 * reading (stock 15,406 / 6,802 / 4,510 pulses in T1[9] / [13] / [17], 734
 * in the band-1 T1[13] calls). [static][measured] */
#define   MT7996_PHY_WFCOMM_3008_CALEND   0x007c00ffu
#define MT7996_PHY_CALEND_US            10u	/* the pulse ends with this wait (V:e0047e8a / V:e004946a) */
#define   MT7996_PHY_RST_CALEND           0x0103ffffu
#define MT7996_PHY_DSP_EF00(band)         MT7996_PHY_BN(band, 0xef00u)	/* 0x8308ef00 */
#define   MT7996_PHY_DSP_EF00_PULSE       HW_BIT(2)

/*
 * CSI (channel state information). [static: ROM names]
 * CSI_CTRL +0x4800: [31] CSI_MEM_IN_BAND0, [30] TOAE_CSI_INPUT_CH_SEL,
 *   [29] RX_FD_TOAE_CAL_MAN_CTRL. Reads 0x80000000 in normal RX. [measured]
 *   The TTG RX calibrations set [29] before every IQ engine run
 *   (0x80000000 -> 0xa0000000). [static][measured]
 * CSI_AHB_CTRL +0x4814: [31] OFDM_CSI_MEM_TOTAL_N_EN, [29:24] TOTAL_N,
 *   [0] OFDM_CSI_AHB_START (start a dump through the AHB bridge).
 * CSI_AHB_STATUS +0x4818 (RO): [31] dump done. Reads 0x20800000 idle,
 *   0xa0804000 after a completed dump. [measured]
 */
#define MT7996_PHY_CSI_CTRL(band)         MT7996_PHY_WFCOMM(band, 0x4800u)
#define   MT7996_PHY_CSI_CTRL_MEM_IN_BAND0 HW_BIT(31)
#define   MT7996_PHY_CSI_CTRL_TOAE_INPUT_CH_SEL HW_BIT(30)
#define   MT7996_PHY_CSI_CTRL_FD_TOAE_CAL_MAN HW_BIT(29)
#define MT7996_PHY_CSI_AHB_CTRL(band)     MT7996_PHY_WFCOMM(band, 0x4814u)
#define   MT7996_PHY_CSI_AHB_TOTAL_N_EN   HW_BIT(31)
#define   MT7996_PHY_CSI_AHB_TOTAL_N      HW_GENMASK(29, 24)
#define   MT7996_PHY_CSI_AHB_START        HW_BIT(0)
#define MT7996_PHY_CSI_AHB_STATUS(band)   MT7996_PHY_WFCOMM(band, 0x4818u)
#define   MT7996_PHY_CSI_AHB_STATUS_DONE  HW_BIT(31)

/*
 * AHB bridge, +0x5000..+0x5054: a PHY -> memory writer with its own
 * start/end address registers. Clients: RDD0..2 (radar pulse records) and
 * CSI_TOAE (CSI SRAM dump). Addresses are MCU-bus addresses; the stock
 * firmware places them in WF_MCU_SYSRAM (0x00400000..0x0042ffff), which the
 * host reads directly. No SYSRAM bank, PLE/PSE partition or stop cycle.
 * [static: ROM names; behaviour measured]
 * Reset/idle values: TOP 0x80 (burst 8, disabled), RDDn_CONFIG0 0xf0
 * (CMP_CNT 15), all addresses 0, RSV1/RSV2 0xffffffff. [measured]
 * Measured CSI dump: CSI_TOAE START/END = a 64 KiB region, TOP = 0x81,
 * CONFIG0 = STALL | ENABLE, TOAE_OUT_LSB[16:14] = 4, CSI_AHB_START = 1 with
 * BN_CLK0/1[29] and CSI_CLK0/1[29] set: exactly 4096 bytes land at START,
 * COMPLETED_WRITE_ADDR = START + 0x1000, TRANSACTION_NUM = 1. Without the
 * [29] clocks nothing lands. Payload: 1024 words of two int16 within
 * +-8192 (14-bit I/Q of the CSI SRAM), all zero unless the normal WM's
 * per-frame CSI processing ran; reception alone does not fill it. Not a
 * sample stream. [measured]
 */
#define MT7996_PHY_AHB(band, off)         MT7996_PHY_WFCOMM(band, 0x5000u + (off))
#define MT7996_PHY_AHB_TOP(band)          MT7996_PHY_AHB(band, 0x00u)
#define   MT7996_PHY_AHB_TOP_SW_RESET     HW_BIT(31)
#define   MT7996_PHY_AHB_TOP_OUTBUF_FLUSH HW_BIT(16)
#define   MT7996_PHY_AHB_TOP_BURST_LEN    HW_GENMASK(7, 4)
#define   MT7996_PHY_AHB_TOP_HALT         HW_BIT(1)
#define   MT7996_PHY_AHB_TOP_ENABLE       HW_BIT(0)
#define MT7996_PHY_AHB_ISR(band)          MT7996_PHY_AHB(band, 0x04u)
/* client k: 0..2 = RDD0..2, 3 = CSI_TOAE */
#define   MT7996_PHY_AHB_ISR_ENABLE(k)    HW_BIT(4 * (k) + 3)
#define   MT7996_PHY_AHB_ISR_STATUS(k)    HW_BIT(4 * (k))
/* Client CONFIG0 / START / END / COMPLETED (RO): RDDn at 0x08 + 0x10*n,
 * CSI_TOAE at 0x38. */
#define MT7996_PHY_AHB_CFG0(band, k)      MT7996_PHY_AHB(band, 0x08u + 0x10u * (k))
#define MT7996_PHY_AHB_START(band, k)     MT7996_PHY_AHB(band, 0x0cu + 0x10u * (k))
#define MT7996_PHY_AHB_END(band, k)       MT7996_PHY_AHB(band, 0x10u + 0x10u * (k))
#define MT7996_PHY_AHB_DONE_ADDR(band, k) MT7996_PHY_AHB(band, 0x14u + 0x10u * (k))
#define   MT7996_PHY_AHB_CLIENT_RDD0      0u
#define   MT7996_PHY_AHB_CLIENT_RDD1      1u
#define   MT7996_PHY_AHB_CLIENT_RDD2      2u
#define   MT7996_PHY_AHB_CLIENT_CSI_TOAE  3u	/* CFG0 0x38, START 0x3c, END 0x40, DONE 0x44 */
#define   MT7996_PHY_AHB_CFG0_TRANS_NUM   HW_GENMASK(31, 16)	/* RO, CSI_TOAE only */
#define   MT7996_PHY_AHB_CFG0_STATE       HW_GENMASK(11, 8)	/* RO, CSI_TOAE only */
#define   MT7996_PHY_AHB_CFG0_CMP_CNT     HW_GENMASK(7, 4)
#define   MT7996_PHY_AHB_CFG0_STALL_EN    HW_BIT(3)
#define   MT7996_PHY_AHB_CFG0_FILL_MODE   HW_BIT(2)	/* EXTMEM_FILL_MODE */
#define   MT7996_PHY_AHB_CFG0_HDR_STAMP   HW_BIT(1)
#define   MT7996_PHY_AHB_CFG0_ENABLE      HW_BIT(0)
/* AHB_RSV0 +0x48: radar control word, not a channel: [0] enable, [5:4] user
 * select (code writes 2), [31:16] length (0x20); no stock caller. RSV1..3
 * +0x4c..+0x54. [static] */
#define MT7996_PHY_AHB_RSV(band, n)       MT7996_PHY_AHB(band, 0x48u + 4u * (n))
#define   MT7996_PHY_AHB_RSV0_EN          HW_BIT(0)
#define   MT7996_PHY_AHB_RSV0_USER_SEL    HW_GENMASK(5, 4)
#define   MT7996_PHY_AHB_RSV0_LEN         HW_GENMASK(31, 16)

/*
 * TOAE accelerator, +0x5bc4..+0x5be8. [static: ROM names]
 * TOAE_ACC_0: [31:30] DEC_PHASE, [29:26] CH_SEL, [25] ACC_MANU, [24]
 *   HE_TONE_MASK_SEL, [23] VHT_TONE_SEL, [22] RATE_IS_32, [21:19] PRIM_CH,
 *   [18] DISABLE_TONE, [17] CLEAR, [16] ROTATOR_BYPASS, [15:12] MODE3..0_EN,
 *   [11:8] FFT_IN_SHIFT, [7:5] BW_SEL, [2:0] PFD.
 * TOAE_ACC_1 [13:0] TAU; TOAE_ACC_2 [30:16] H_PEAK_I, [14:0] H_PEAK_Q.
 * R0..R6 (RO): H_PEAK_I [23:0], H_PEAK_Q [23:0], H_INDEX [27:18],
 *   FSM [7:4] / MODE_DONE [3:0], ERR [27:0], H_PEAK_ABS [30:0], T_PEAK_TAU [13:0].
 */
#define MT7996_PHY_TOAE_ACC(band, n)      MT7996_PHY_WFCOMM(band, 0x5bc4u + 4u * (n))	/* n 0..2 */
#define MT7996_PHY_TOAE_ACC_R(band, n)    MT7996_PHY_WFCOMM(band, 0x5bd0u + 4u * (n))	/* n 0..6 */
#define   MT7996_PHY_TOAE_ACC0_CH_SEL     HW_GENMASK(29, 26)
#define   MT7996_PHY_TOAE_ACC0_ROT_BYPASS HW_BIT(16)
#define   MT7996_PHY_TOAE_ACC0_MODE_EN    HW_GENMASK(15, 12)
#define   MT7996_PHY_TOAE_ACC0_FFT_IN_SHIFT HW_GENMASK(11, 8)
#define   MT7996_PHY_TOAE_ACC0_BW_SEL     HW_GENMASK(7, 5)

/*
 * Background (monitor) receiver front end, band 0 (chain 4). The RDD2
 * start programs these at FIXED band-0 addresses whatever the band index.
 * [static] Capturing chain 4 with these set has not produced samples.
 * [measured]
 *  +0x5c00 [0]: enable.
 *  +0x5df0 [3:0]: analog bandwidth code (same code as BGRX_FE[30:28]);
 *          [19:10] = 0xb0 on first use.
 *  +0x5df4 [31:30]: pulsed (set, cleared).
 *  +0x5dfc [28]: IPI reset. [static: mt76 testmode]
 *  +0x5e10: TOAE bandwidth. [static]
 */
#define MT7996_PHY_BGRX_EN(band)          MT7996_PHY_WFCOMM(band, 0x5c00u)
#define   MT7996_PHY_BGRX_EN_ON           HW_BIT(0)
#define MT7996_PHY_BGRX_CFG(band)         MT7996_PHY_WFCOMM(band, 0x5df0u)
#define   MT7996_PHY_BGRX_CFG_BW          HW_GENMASK(3, 0)
#define   MT7996_PHY_BGRX_CFG_19_10       HW_GENMASK(19, 10)
/* BGRX_CFG (band 0 WF_COMM +0x5df0): the detector start's first use (WM byte
 * 0xe01ec27a clear; V:e00caacc with en = 1, then V:e00cab5c) writes, one read-modify-write
 * each: [6:4] = !en, (RX_CTRL1 IPI_EN = 5,) [9] = en, [8] = en, [7] = !en, [19:10] = 0xb8,
 * [29:20] = !en; then [19:10] = 0xb0. Stock 0x00028300 -> 0x0002e300 -> 0x0002c300.
 * [static][measured] */
#define   MT7996_PHY_BGRX_CFG_F29_20      HW_GENMASK(29, 20)
#define   MT7996_PHY_BGRX_CFG_B9          HW_BIT(9)
#define   MT7996_PHY_BGRX_CFG_B8          HW_BIT(8)
#define   MT7996_PHY_BGRX_CFG_B7          HW_BIT(7)
#define   MT7996_PHY_BGRX_CFG_F6_4        HW_GENMASK(6, 4)
#define MT7996_PHY_BGRX_5DF4(band)        MT7996_PHY_WFCOMM(band, 0x5df4u)
/* BGRX_5DF4 (band 0 WF_COMM +0x5df4): [30] then [31] set, then (after RDD_2148 [20])
 * [30] then [31] cleared (V:e00caa8c with en 1, then 0); first use only (V:e00caacc) [29:21] =
 * 0x1f8. Stock 0x00100000 -> 0x3f100000. [static][measured] */
#define   MT7996_PHY_BGRX_5DF4_B31        HW_BIT(31)
#define   MT7996_PHY_BGRX_5DF4_B30        HW_BIT(30)
#define   MT7996_PHY_BGRX_5DF4_F29_21     HW_GENMASK(29, 21)
#define MT7996_PHY_IPI_RST(band)          MT7996_PHY_WFCOMM(band, 0x5dfcu)
#define   MT7996_PHY_IPI_RST_B28          HW_BIT(28)
#define MT7996_PHY_TOAE_BW(band)          MT7996_PHY_WFCOMM(band, 0x5e10u)

/* WF_COMM +0x705c [27:24], +0x72c8 [17:12], +0x7830 [25]: SW-LNA profile
 * fields (band AGC). [static] The per-band PHY init's register table writes
 * 82 words of this block (WFC_AGC, phy_map.h MT7996_PHY_REGTBL_*), among
 * them +0x7830 = 0x4b500333. [static][measured] */
#define MT7996_PHY_WFCOMM_705C(band)      MT7996_PHY_WFCOMM(band, 0x705cu)
#define MT7996_PHY_WFCOMM_72C8(band)      MT7996_PHY_WFCOMM(band, 0x72c8u)
#define MT7996_PHY_WFCOMM_7830(band)      MT7996_PHY_WFCOMM(band, 0x7830u)

/* WF_COMM +0xb580: the per-band PHY init writes [24:23] = 1 on E1 parts
 * only (MT7996_CBTOP_HW_REV == 0x8a00, V:e00defba; not executed on this
 * unit); every channel set writes it three times: PHY table A whole, then
 * its explicit field [13:4] = 0 (not on E1 parts; phy_map.h), then the
 * channel-set D-die RX calibration [24:23] = 1 on every die revision
 * (V:e00de3fc; 0x01000007 -> 0x00800007 on all three bands; phy_cal.h).
 * +0xc0a4 = 0x1fff0704 (write only); +0xc0fc = 0x2d, later [7] = 1 (0xad;
 * T2{8} writes it again); +0xc104 [31:30] = 3 (0xa000 -> 0xc000a000). All
 * by the per-band PHY init (below). At every channel set PHY table B
 * writes +0xc104 [3:0] = 4 on band 2 at BW class 4 when EEPROM 0x1a4 [6]
 * is set, else 0 (V:e00d9824 / V:e00d9858; stock 0xc000a000, unchanged).
 * [static][measured] */
#define MT7996_PHY_WFCOMM_B580(band)      MT7996_PHY_WFCOMM(band, 0xb580u)
#define   MT7996_PHY_WFCOMM_B580_F24_23   HW_GENMASK(24, 23)
#define     MT7996_PHY_WFCOMM_B580_F24_23_VAL 1u	/* the init (E1 parts, e00defce) and every channel set */
#define   MT7996_PHY_WFCOMM_B580_F13_4    HW_GENMASK(13, 4)
#define MT7996_PHY_WFCOMM_C0A4(band)      MT7996_PHY_WFCOMM(band, 0xc0a4u)
#define   MT7996_PHY_WFCOMM_C0A4_INIT     0x1fff0704u	/* e00deae2 / e00deae8, whole word */
#define MT7996_PHY_WFCOMM_C0FC(band)      MT7996_PHY_WFCOMM(band, 0xc0fcu)
#define   MT7996_PHY_WFCOMM_C0FC_B7       HW_BIT(7)
#define   MT7996_PHY_WFCOMM_C0FC_INIT     0x0000002du	/* e00dead2, whole word */
#define MT7996_PHY_WFCOMM_C104(band)      MT7996_PHY_WFCOMM(band, 0xc104u)
#define   MT7996_PHY_WFCOMM_C104_F31_30   HW_GENMASK(31, 30)
#define   MT7996_PHY_WFCOMM_C104_F31_30_INIT 3u	/* e00df030 */
#define   MT7996_PHY_WFCOMM_C104_F3_0     HW_GENMASK(3, 0)

/*
 * WF_COMM +0xd000 words with known writers that dfe.h does not cover.
 * [static unless marked]
 *  +0xd208 [1] = 1 (0 -> 2) and +0xd4cc [15:8] = 0x1c (0x00041401 ->
 *          0x00041c01), +0xd4f0 [31] = 1 (0x20000007 -> 0xa0000007): the
 *          per-band PHY init (below), every band. [measured]
 *  +0xd214 = 0xe1, +0xd418 = 0: rows of the per-band PHY init's register
 *          table (phy_map.h MT7996_PHY_REGTBL_*). [measured]
 *  +0xd230 [5:4], +0xd234 [8], +0xd240 [14:0], +0xd24c [15]: WM PHY
 *          preparation on bands 1 and 2 (phy_cal.h); +0xd24c is also
 *          read-modified by the channel-set D-die RX calibration on bands 1
 *          and 2 (0, unchanged). The per-band PHY init
 *          writes the preparation's mode-16 values on bands 1 and 2
 *          (V:e00df274..): +0xd234 [8] = 1 (0xe02 -> 0xf02), +0xd230 [5:4] =
 *          1 (0x406 -> 0x416), +0xd240 [14:0] = 0x11d4 (0x111411f4 ->
 *          0x111411d4); the calibration items' PHY preparation
 *          (T1{11,13,15..17,21,22}, T2{0,4,5,6}) read-modifies all three
 *          again. [measured]
 *  +0xd250: DPD (T2{0..3}, phy_cal.h) sets [16] on band 1 (WF 0/1, every
 *          step, restored); 0x10400 on band 0 only on its LDPD path (MT7976
 *          WF3, EEPROM 0x1a9 [7]), which the stock load does not run.
 *  +0xd400..+0xd414: item 0's RF table key records and RF-interface word
 *          (below).
 *  +0xd430, +0xd4c0, +0xd7a8, +0xda14: DPD (phy_cal.h); +0xd434 and
 *          +0xd7d8..+0xd7e8: firmware words of the calibration init
 *          (phy_cal.h MT7996_PHY_FW_D434, MT7996_PHY_FW_D7D8 / _EECFG);
 *          +0xd49c: below (the TTG RX engine block).
 *  +0xd480 [20], +0xd874 [28]: set by WM PHY preparation; +0xd480 [0]
 *          cleared by the DFE path setup (V:e0048b8a; observed
 *          0x4402000c -> 0x4412000c in the TTG RX calibrations). The
 *          per-band PHY init clears +0xd480 [20] (V:e00debc4; 0x4412000c ->
 *          0x4402000c) and its TOP init pulses [19] (set V:e00dc02c, clear
 *          V:e00dc044); the calibration items read-modify it (V:e0048b9e,
 *          V:e00490ba). [measured]
 *  +0xd4a0: the TOP init (adie.h) sets [22] (MT7976), or [20] = 1, [21] =
 *          0, [22] = 1, then [22] and [20] again (MT7977); observed
 *          0x00130801 -> 0x00530801. T2{6} read-modifies it (phy_cal.h).
 *          [measured]
 *  +0xd4d0: chain/lane enables [inferred]: four byte fields, [8n+4] and
 *          [8n+3:8n] (n 0..3; per chain [inferred]). WM PHY init writes
 *          0x01010101 (read back 0x01010101 with the firmware up [measured]);
 *          preparation writes 0x1010101f; the register helper builds [3:0],
 *          [4], [11:8] (chain one-hot or 0xf), [12], [19:16], [20], [27:24],
 *          [28] by mode. Every calibration tail (V:e0048b26) RMWs it to
 *          0x10101010 ([8n+4] = 1, [8n+3:8n] = 0) right before the D-die list
 *          restore, which holds this word and puts the backed-up 0x01010101
 *          back. [static][measured]
 *  +0xd4d8: WM PHY init writes 0x01010101 [measured: reads back].
 *  +0xd4e4: preparation sets [11:8] = 0xf and clears [23:12].
 *  +0xd750 (band 0, 0x830ad750): read by the WM's 500 ms monitor timer
 *          (V:e011c020 -> V:e00e43ae); negative values are logged. Stock 0.
 *          [guess: a firmware-maintained PHY status word] [measured]
 *  +0xd760 [11:0], +0xd798: DPD state / status (phy_cal.h); band 0's
 *          +0xd760..+0xd790 also hold the T2{6} records of all bands.
 *  +0xd6c4 (DSP lock), +0xd6e8..+0xd704 (OTFIQ configuration), +0xd824,
 *          +0xd9c0, +0xd9c4 (TSSI measurement; +0xd9c0 is also the TTG RX
 *          IQ engine's chain select): phy_cal.h.
 *  +0xd82c..+0xd9bc: the TTG RX DC and IQ engines (below).
 *  +0xdd24 [3:0] = 4, +0xdd28 |= 0x00380000, +0xdd38 |= 0x18000000: written
 *          (with dfe.h's +0xc00 [19:18] = 3) by the code that follows the
 *          T1[23] body in the item list, with the CONN AFE +0x14 [14] write
 *          (V:e004b4b4, mode 0xf [inferred]); not by T1[23] itself, whose
 *          decoded accesses include none of them. [static]
 * Background (monitor) receiver enables, band-0 fixed addresses, written by
 * the RDD2 start:
 *  BGRX_FE0 0x830ada20: [25] enable (DPD's LDPD path writes 0x1a100001
 *           whole; band 1's DPD sets 0x831ada20 [11:8] = 3).
 *  BGRX_FE  0x830ada2c: [30:28] bandwidth code, [25], [24], [15] enables,
 *           [14:9] = !en (0 when enabling: stock 0x03009180 -> 0x03008180;
 *           V:e00caca4) (DPD's LDPD path writes 0x33008180 whole; band 1's
 *           DPD sets 0x831ada2c [25]). The LDPD path does not run in the
 *           stock load; the band-1 writes do (phy_cal.h T2{0..3}).
 */
#define MT7996_PHY_WFCOMM_D208(band)      MT7996_PHY_WFCOMM(band, 0xd208u)
#define   MT7996_PHY_WFCOMM_D208_B1       HW_BIT(1)
#define MT7996_PHY_WFCOMM_D230(band)      MT7996_PHY_WFCOMM(band, 0xd230u)
#define   MT7996_PHY_WFCOMM_D230_F5_4     HW_GENMASK(5, 4)
#define   MT7996_PHY_WFCOMM_D230_F5_4_INIT 1u	/* e00df29a, bands 1 and 2 */
#define MT7996_PHY_WFCOMM_D234(band)      MT7996_PHY_WFCOMM(band, 0xd234u)
#define   MT7996_PHY_WFCOMM_D234_B8       HW_BIT(8)
#define MT7996_PHY_WFCOMM_D240(band)      MT7996_PHY_WFCOMM(band, 0xd240u)
#define   MT7996_PHY_WFCOMM_D240_LO       HW_GENMASK(14, 0)
#define   MT7996_PHY_WFCOMM_D240_INIT     0x11d4u	/* mode 16; 0x11f4 otherwise */
#define MT7996_PHY_WFCOMM_D480(band)      MT7996_PHY_WFCOMM(band, 0xd480u)
#define   MT7996_PHY_WFCOMM_D480_B20      HW_BIT(20)
#define   MT7996_PHY_WFCOMM_D480_B19      HW_BIT(19)
#define MT7996_PHY_WFCOMM_D4A0(band)      MT7996_PHY_WFCOMM(band, 0xd4a0u)
#define   MT7996_PHY_WFCOMM_D4A0_B22      HW_BIT(22)
#define   MT7996_PHY_WFCOMM_D4A0_B21      HW_BIT(21)
#define   MT7996_PHY_WFCOMM_D4A0_B20      HW_BIT(20)
#define MT7996_PHY_WFCOMM_D4CC(band)      MT7996_PHY_WFCOMM(band, 0xd4ccu)
#define   MT7996_PHY_WFCOMM_D4CC_F15_8    HW_GENMASK(15, 8)
#define   MT7996_PHY_WFCOMM_D4CC_F15_8_INIT 0x1cu	/* e00debd8 / e00debde */
#define MT7996_PHY_WFCOMM_D4D0(band)      MT7996_PHY_WFCOMM(band, 0xd4d0u)
#define   MT7996_PHY_WFCOMM_D4D0_EN(n)    HW_BIT(8u * (n) + 4u)
#define   MT7996_PHY_WFCOMM_D4D0_SEL(n)   HW_GENMASK(8u * (n) + 3u, 8u * (n))
#define   MT7996_PHY_WFCOMM_D4D0_LANES  4u	/* n 0..3; the DCOC tail sets EN and clears SEL of lane 3, 2, 1, then 0 (V:e0048b26) */
#define   MT7996_PHY_WFCOMM_D4D0_INIT     0x01010101u
#define   MT7996_PHY_WFCOMM_D4D0_CALTAIL  0x10101010u
#define MT7996_PHY_WFCOMM_D4D8(band)      MT7996_PHY_WFCOMM(band, 0xd4d8u)
#define MT7996_PHY_WFCOMM_D4E4(band)      MT7996_PHY_WFCOMM(band, 0xd4e4u)
#define MT7996_PHY_WFCOMM_D4F0(band)      MT7996_PHY_WFCOMM(band, 0xd4f0u)
#define   MT7996_PHY_WFCOMM_D4F0_B31      HW_BIT(31)
#define MT7996_PHY_WFCOMM_D750            0x830ad750u	/* band 0 */
#define MT7996_PHY_BGRX_FE0               0x830ada20u
#define   MT7996_PHY_BGRX_FE0_EN          HW_BIT(25)
#define MT7996_PHY_BGRX_FE                0x830ada2cu
#define   MT7996_PHY_BGRX_FE_BW           HW_GENMASK(30, 28)
#define   MT7996_PHY_BGRX_FE_EN25         HW_BIT(25)
#define   MT7996_PHY_BGRX_FE_EN24         HW_BIT(24)
#define   MT7996_PHY_BGRX_FE_EN15         HW_BIT(15)
#define   MT7996_PHY_BGRX_FE_F14_9        HW_GENMASK(14, 9)	/* = !en by the RDD start (V:e00caca4) */

/* WF_COMM +0xd400 + 4 WF (WF 0..4, +0xd400..+0xd410): RF table key records
 * [name guess]. RF_INIT (calibration item 0, adie.h "RF_INIT") zeroes all
 * five (MT7976 V:e0051062..V:e00510a2, MT7977 V:e0051c46..V:e0051c84); then
 * the RF table helpers write, per WF, the half [15:0] (P[2] = SX 0) or
 * [31:16] (SX != 0) = key + (nibble-swapped (key + 1)) << 8, key = the table
 * column the walker used (MT7976 RF_COMMON V:e0051146..V:e00511a2: the
 * position of P[1] * 5 + WF in 0xe0074fac; MT7977 item 1 RF COMMON
 * V:e005205e..V:e00520be: the position of group * 5 + WF + (ECO < 10) * 0x78
 * in 0xe007a1c8). Stock: band 0 0x1000, 0x2001, 0x3002, 0x4003 (WF 0..3);
 * bands 1 / 2 per group, e.g. 0x4003. No reader in the WM found: bookkeeping
 * for the DSP or a debugger [inferred]. [static][measured] */
#define MT7996_PHY_RF_TBL_KEY(band, wf)   MT7996_PHY_WFCOMM(band, 0xd400u + 4u * (wf))
#define   MT7996_PHY_RF_TBL_KEY_SX0       HW_GENMASK(15, 0)
#define   MT7996_PHY_RF_TBL_KEY_SX1       HW_GENMASK(31, 16)
/* The record as a value of the column key, for the half [15:0] (P[2] = 0; MT7976
 * RF_COMMON V:e0051146..V:e00511a2). Stock band 0: 0x1000, 0x2001, 0x3002, 0x4003 for WF 0..3.
 * [static][measured] */
#define MT7996_PHY_RF_TBL_KEY_REC(key) \
	(((key) + (((((key) + 1u) & 0xfu) << 4 | (((key) + 1u) >> 4 & 0xfu)) << 8)) & 0xffffu)

/* WF_COMM +0xd414: RF-interface word written by item 0 (0x83Xad400..414)
 * before any table: MT7976 0xe3220503 (V:e005105a); MT7977 0x77e40312 when
 * the ECO byte (die 2's TOP 0x02c [7:0], phy_cal.h "Calibration init") > 9,
 * else 0x77e20312 (V:e0051c2a..V:e0051c3a). Observed 0xe3220503 (band 0),
 * 0x77e40312 (bands 1, 2). Read twice by V:e0047ec0 at every SX tune and
 * every calibration body: [27:16] == 0xbb1 with [12] set enables the WM's
 * "Cal-> [n]" debug line (the same key as DBG_OVR; the SX callers pass code
 * 0, which logs nothing). The RF COMMON table helpers read it twice first
 * (MT7976 RF_COMMON V:e0051110 / V:e0051112, MT7977 item 1 RF COMMON
 * V:e0052026 / V:e0052028 and COMMON_BW V:e0051df8 / V:e0051dfa) and skip
 * their tables when [27:16] == 0xbb1 and [0]. Item 1's P[7] / P[6] / group
 * helpers (rfchan.h RFCHAN_XLNA / _XPA / _*_GROUP) and the MT7977 SX tables
 * do not read it; the MT7976 SX (item 10, V:e005ee7e / V:e005ee80) reads it
 * twice more and ignores the value. The same key gates the calibration
 * bodies' debug paths: T2{4} zeroes every chain's DNL words and returns when
 * [27:16] == 0xbb1 and [29]; T2{5} returns when [27:16] == 0xbb1 and [28];
 * T2{6} takes a debug path on [31] and logs on [3]; DPD (T2{0..3}) has debug
 * exits and the WM scratch overrides 0x8301fc14.. under the same key
 * (phy_cal.h). With the key and [13] the channel set skips the walks of
 * both channel-set PHY tables (phy_map.h MT7996_PHY_CHTBL_*; V:e00d9276 ->
 * V:e00d9476, V:e00d973e -> V:e00d97f4) and the table fields that follow
 * them up to those targets; the later fields still run. Only the tests of
 * [27:16], [13], [12], [0], [28], [29], [31] and [3] are known; whether the
 * other bits configure hardware is not established, so an own firmware
 * writes the word as MediaTek does. [static][measured] */
#define MT7996_PHY_RFIF_D414(band)        MT7996_PHY_WFCOMM(band, 0xd414u)
#define   MT7996_PHY_RFIF_D414_LOGKEY     HW_GENMASK(27, 16)
#define   MT7996_PHY_RFIF_D414_LOGKEY_ON  0xbb1u
#define   MT7996_PHY_RFIF_D414_ZERO_DNL   HW_BIT(29)
#define   MT7996_PHY_RFIF_D414_SKIP_TSSI  HW_BIT(28)
#define   MT7996_PHY_RFIF_D414_SKIP_CHTBL HW_BIT(13)	/* with LOGKEY_ON */
#define   MT7996_PHY_RFIF_D414_LOG        HW_BIT(12)
#define   MT7996_PHY_RFIF_D414_SKIP_TBL   HW_BIT(0)	/* with LOGKEY_ON */
#define   MT7996_PHY_RFIF_D414_MT7976     0xe3220503u
#define   MT7996_PHY_RFIF_D414_MT7977     0x77e40312u	/* ECO > 9 */
#define   MT7996_PHY_RFIF_D414_MT7977_ECO9 0x77e20312u	/* ECO <= 9 (V:e0051cda) */

/*
 * WF_COMM +0xd49c..+0xd9bc: the TTG RX DC and IQ engines (phy_cal.h
 * T1{18..23}; mode 0x13 = T1{18..22}, mode 0x16 = T1[23]; the IQ engine
 * also in the TX IQ calibration, mode 0xc = T1{8,12,16}) [guess: names].
 * Per-chain status and results: RFINTF_DCE_STS, RFINTF_IQE_STS,
 * RFINTF_IQBIN, MT7996_PHY_RXIQC_*, MT7996_PHY_FIQ_* (above).
 * [static][measured]
 *  +0xd49c: front-end mode word, [2:0] = 3 for the DC phase, 1 for the IQ
 *           phase (both modes; V:e0049b40 maps mode 4 -> 3, 0x13 -> 1, 0xc
 *           -> 0, 2 -> 5, as DPD uses it, phy_cal.h). Observed 0x400001c6.
 *           The per-band PHY init's TOP init (adie.h) sets [30] = 1, [31] =
 *           0 (MT7976), or [30] = 1, [31] = 0, later [30] = 1, [31] = 1
 *           (MT7977): after it 0x400001c6 on band 0, 0xc00001c6 on bands 1
 *           and 2.
 *  +0xd82c: [18:16] = 1 (DC engine); before the IQ engine [31] = 0 (mode
 *           0x13) or [31] = 1, [29] = 0 (mode 0x16). Observed 0x29200100.
 *  +0xd844: IQ engine control: [23:16] = 0x4f, [0] = 1, [14] = 1; run: [2]
 *           = 1, wait RFINTF_IQE_STS, [3] = 1, [2] = 0, [3] = 0.
 *           0x32100001 -> 0x324f4001. Table fit (V:e004a11e, after loading
 *           the bins): [12] = 0, [14] = 0, [2] = 1, 20 us, [7] = 1, 20 us,
 *           [3] = 1, [14] = 1, [7] = 0, [3:2] = 0 (no status poll).
 *  +0xd848: [17] = 1, [24:20] = 4/5/9/0x11/0x17 for bw 0..4 (above
 *           2.4 GHz; 2.4 GHz 4), [29:28] = 0, [6] = 0. [15:8] bin index and
 *           [2] bin strobe for RFINTF_IQBIN (read and load).
 *  +0xd84c: [5:0] = 0x1a, [13:8] = 0 (observed 0x41a).
 *  +0xd850: [3:2] = sp8f (2 for bw 0, 1; 1 for bw 2 above 2.4 GHz; 0 for
 *           160/320; rewritten at the end of V:e004a11e), [1] = 0, [0] =
 *           (bw == 0), [5:4] = 2, [6] = 1, [15:8] = 2, [19] = 1; IQ phase
 *           [18:16] = 3. 0x00060000 -> 0x000b0269.
 *  +0xd860: DC engine start [0] (V:e00498fc, chains 0..3, also on band 2:
 *           chain 4 gets no DC engine run but still gets IQ results),
 *           cleared with [1], [2] when every chain is idle.
 *  +0xd890 [3:0], +0xd898 [1:0], +0xd8a0 [4:0] = 0 (mode 0x16, every step
 *           and chain).
 *  +0xd8c0 [3:0], +0xd8c8 [1:0] = 0; +0xd8d0 [4:0] = 0 (mode 0x13) or the
 *           step table (mode 0x16, phy_cal.h MT7996_PHY_TTG_RX_FIQ_D8D0_*).
 *           These are entry 0 of the gain-index selectors
 *           (MT7996_PHY_GSEL_*, below).
 *  +0xd9b4: per-bin table data: one entry per bin, the RFINTF_IQBIN format.
 *  +0xd9b8: per-bin table access: [1] = 1 (table mode, set for reading and
 *           loading), [0] write strobe (1 then 0, 5 us each) per loaded bin.
 *  +0xd9bc: [5:4] = 1, [0] = 1; IQ phase [17:16] = 2, [11:8] = 5 (6 for
 *           6 GHz above 5.9 GHz when P[8] == 2; 9 in mode 0x16), [12] = 0,
 *           [3:1] = 4 (1 in mode 0x16), [25:20] = 9/8/8/0x18/0x18 for bw
 *           0..4 (table 0xe0069374[sp8f * 5 + bw]), [13] = 1 (0 in mode
 *           0x16). 0x00026598 -> 0x00926599.
 *  +0xd9c0: chain select (phy_cal.h MT7996_PHY_TSSI_MEAS_CTL; [27] while
 *           the per-bin table is read or loaded).
 * TX IQ mode 0xc (phy_cal.h T1{8,12,16}), the same engine on the TX
 * loopback:
 *  +0xd844: [23:16] = 3, [27:24] = 0xb, [0] = 1, [15] = 1; run [2], end [3]
 *           pulse then [2] = 0. 0x32100001 -> 0x3b038001.
 *  +0xd848: [6] = 0, [17] = 1, [24:20] = 5 (group 0), 9 (MT7977, class 0),
 *           0x11 (class 3 / 4; MT7976 above 2.4 GHz); [1] = 1 while
 *           reading bins.
 *  +0xd84c: [5:0] = 0x1a, [13:8] = 0.
 *  +0xd850: [5:4] = 3, [3:2] = 0, [1] = 0, [0] = 0, then [18:16] = 5, [19] =
 *           0.
 *  +0xd85c: [11:0] = 0x13 (0x27 for class 4) [guess: engine length].
 *  +0xd870: [19] = 1. +0xd9bc [0] = 0. +0xd9c0 [19:16] = 0, [31:28] = 1.
 *           +0xd9b8 [1] = 1 while reading bins.
 * Other band words of these calibrations (above): WF_COMM +0x3008 [4],
 * +0x3020, CSI_CTRL [29], BN +0x0050, the reset pulse V:e0047e36, BN
 * +0xf100 [0] = 0 and WF_RXFE_01 [19] = 0 in mode 0x16, RXFE_CTRL [19] = 0;
 * TPC_MCS_CTL_00 (BN +0xe764) [9] = 1, [8] = 0, TSSI_CORE_00 PKT_MODE = 0
 * and the band tone generator's load pulse BN +0xfc0c [0] with its twin
 * (phy_cal.h MT7996_PHY_TSSI_CORE0, MT7996_PHY_TONEGEN_CTL).
 */
#define MT7996_PHY_WFCOMM_D49C(band)      MT7996_PHY_WFCOMM(band, 0xd49cu)
#define   MT7996_PHY_WFCOMM_D49C_MODE     HW_GENMASK(2, 0)
#define   MT7996_PHY_WFCOMM_D49C_B31      HW_BIT(31)	/* MT7977 TOP init */
#define   MT7996_PHY_WFCOMM_D49C_B30      HW_BIT(30)
#define MT7996_PHY_DCE_CTRL(band)         MT7996_PHY_WFCOMM(band, 0xd82cu)
#define   MT7996_PHY_DCE_CTRL_SEL         HW_GENMASK(18, 16)
#define   MT7996_PHY_DCE_CTRL_B29         HW_BIT(29)
#define   MT7996_PHY_DCE_CTRL_B31         HW_BIT(31)
#define MT7996_PHY_IQE_CTRL(band)         MT7996_PHY_WFCOMM(band, 0xd844u)
#define   MT7996_PHY_IQE_CTRL_EN          HW_BIT(0)
#define   MT7996_PHY_IQE_CTRL_START       HW_BIT(2)
#define   MT7996_PHY_IQE_CTRL_LATCH       HW_BIT(3)	/* [guess] */
#define   MT7996_PHY_IQE_CTRL_FIT         HW_BIT(7)	/* [guess] */
#define   MT7996_PHY_IQE_CTRL_B12         HW_BIT(12)
#define   MT7996_PHY_IQE_CTRL_B14         HW_BIT(14)
#define   MT7996_PHY_IQE_CTRL_B15         HW_BIT(15)	/* TX IQ mode */
#define   MT7996_PHY_IQE_CTRL_PARAM       HW_GENMASK(23, 16)
#define   MT7996_PHY_IQE_CTRL_2724        HW_GENMASK(27, 24)	/* 0xb in the TX IQ mode */
#define MT7996_PHY_IQE_CFG0(band)         MT7996_PHY_WFCOMM(band, 0xd848u)
#define   MT7996_PHY_IQE_CFG0_BWSEL       HW_GENMASK(24, 20)
#define   MT7996_PHY_IQE_CFG0_BIN         HW_GENMASK(15, 8)
#define   MT7996_PHY_IQE_CFG0_BIN_STROBE  HW_BIT(2)
#define   MT7996_PHY_IQE_CFG0_BIN_READ    HW_BIT(1)	/* TX IQ mode, while reading bins */
#define MT7996_PHY_IQE_CFG1(band)         MT7996_PHY_WFCOMM(band, 0xd84cu)
#define MT7996_PHY_IQE_CFG2(band)         MT7996_PHY_WFCOMM(band, 0xd850u)
#define   MT7996_PHY_IQE_CFG2_BW20        HW_BIT(0)
#define   MT7996_PHY_IQE_CFG2_RATE        HW_GENMASK(3, 2)	/* sp8f */
#define   MT7996_PHY_IQE_CFG2_IQ          HW_GENMASK(18, 16)
#define MT7996_PHY_IQE_CFG4(band)         MT7996_PHY_WFCOMM(band, 0xd85cu)	/* [guess: name] */
#define   MT7996_PHY_IQE_CFG4_LEN         HW_GENMASK(11, 0)
#define MT7996_PHY_DCE_START(band)        MT7996_PHY_WFCOMM(band, 0xd860u)
#define   MT7996_PHY_DCE_START_GO         HW_BIT(0)
#define MT7996_PHY_WFCOMM_D870(band)      MT7996_PHY_WFCOMM(band, 0xd870u)
#define   MT7996_PHY_WFCOMM_D870_B19      HW_BIT(19)
#define MT7996_PHY_IQE_STEP(band)         MT7996_PHY_WFCOMM(band, 0xd8d0u)
#define   MT7996_PHY_IQE_STEP_VAL         HW_GENMASK(4, 0)
#define MT7996_PHY_IQBIN_DATA(band)       MT7996_PHY_WFCOMM(band, 0xd9b4u)
#define MT7996_PHY_IQBIN_ACC(band)        MT7996_PHY_WFCOMM(band, 0xd9b8u)
#define   MT7996_PHY_IQBIN_ACC_WR         HW_BIT(0)
#define   MT7996_PHY_IQBIN_ACC_TABLE      HW_BIT(1)
#define MT7996_PHY_IQE_CFG3(band)         MT7996_PHY_WFCOMM(band, 0xd9bcu)
#define   MT7996_PHY_IQE_CFG3_BWSEL       HW_GENMASK(25, 20)
#define   MT7996_PHY_IQE_CFG3_N           HW_GENMASK(11, 8)

/*
 * Gain-index selectors of the TTG RX DC / IQ compensation [guess: names and
 * the per-gain reading], WF_COMM, two sets of nine words: set 0 at +0xd890,
 * set 1 at +0xd8c0. MAP (2 words, 16 x 4-bit), GRP (2 words, 16 x 4-bit
 * codes), STEP (5 words, 20 x byte, 5-bit codes). The TTG RX calibrations
 * force entry 0 of each (+0xd890 [3:0], +0xd898 [1:0], +0xd8a0 [4:0],
 * +0xd8c0 [3:0], +0xd8c8 [1:0]; +0xd8d0 [4:0] = the per-gain RX IQ step in
 * mode 0x16, MT7996_PHY_IQE_STEP, which is MT7996_PHY_GSEL_STEP(band, 1,
 * 0)), so in normal reception these words choose, per RX gain index, which
 * per-gain compensation entry applies. [static]
 * Per-band PHY init values (V:e00de622, whole words, write only):
 *   MAP (both sets): 0x76543210, 0xfedcba98 (identity);
 *   AdieSel 5 / 6 (MT7976, band 0; V:e00de9e2): GRP0 0x21111000,
 *     0x00020022; STEP0 0x06090908, 0x05040707, 0x03030205, 0x000a0100
 *     (+0xd8b0 not written); GRP1 0x10000000, 0x00010011; STEP1 0x04000505,
 *     0x03030004, 0x00020200, 0x00000001 (+0xd8e0 not written);
 *   other (MT7977, bands 1 and 2; V:e00df2cc): GRP0 0x01110000, 0; STEP0
 *     0x080b0b0a, 0x07060909, 0x05050407, 0x00030302, 0x00000101; GRP1 0,
 *     0; STEP1 0x04000005, 0x00030000, 0x00000200, 0x00000001.
 * Every MT7977 channel set (the channel-set D-die RX calibration, phy_cal.h;
 * AdieSel byte 7) rewrites set 1 with the init values above (V:e00de28c..,
 * whole words), and at BW320 then read-modifies it to GRP1 0x11110000, 0,
 * STEP1 0x08000909, 0x07060008, 0x00050400, 0x302 with STEP1 [4] (+0xd8e0)
 * = 1; the channel set writes neither set 0 nor, on the MT7976, set 1. The
 * TTG RX items
 * (T1{21,22}) read-modify +0xd8c0 / +0xd8c8 / +0xd8d0. AdieSel: phy_map.h
 * MT7996_PHY_FCB_CFG_INIT. [static][measured]
 */
#define MT7996_PHY_GSEL_MAP(band, set, n)  MT7996_PHY_WFCOMM(band, 0xd890u + 0x30u * (set) + 4u * (n))	/* n 0..1 */
#define MT7996_PHY_GSEL_GRP(band, set, n)  MT7996_PHY_WFCOMM(band, 0xd898u + 0x30u * (set) + 4u * (n))	/* n 0..1 */
#define MT7996_PHY_GSEL_STEP(band, set, n) MT7996_PHY_WFCOMM(band, 0xd8a0u + 0x30u * (set) + 4u * (n))	/* n 0..4 */
#define   MT7996_PHY_GSEL_MAP_INIT        { 0x76543210u, 0xfedcba98u }
#define   MT7996_PHY_GSEL0_GRP_MT7976     { 0x21111000u, 0x00020022u }
#define   MT7996_PHY_GSEL0_STEP_MT7976    { 0x06090908u, 0x05040707u, 0x03030205u, 0x000a0100u }
#define   MT7996_PHY_GSEL1_GRP_MT7976     { 0x10000000u, 0x00010011u }
#define   MT7996_PHY_GSEL1_STEP_MT7976    { 0x04000505u, 0x03030004u, 0x00020200u, 0x00000001u }
#define   MT7996_PHY_GSEL0_GRP_MT7977     { 0x01110000u, 0x00000000u }
#define   MT7996_PHY_GSEL0_STEP_MT7977    { 0x080b0b0au, 0x07060909u, 0x05050407u, 0x00030302u, 0x00000101u }
#define   MT7996_PHY_GSEL1_GRP_MT7977     { 0x00000000u, 0x00000000u }
#define   MT7996_PHY_GSEL1_STEP_MT7977    { 0x04000005u, 0x00030000u, 0x00000200u, 0x00000001u }
/* The gain-index selector set the channel set rewrites (set 1 at
 * +0xd8c0) and its BW320 values: V:e00de316..V:e00de398 write, after the per-band init values
 * (MT7996_PHY_GSEL_MAP_INIT, MT7996_PHY_GSEL1_GRP_MT7977, MT7996_PHY_GSEL1_STEP_MT7977),
 * MAP_INIT again, then these, whole words (no read): GRP 2 words, STEP 5 words (STEP [4] =
 * +0xd8e0, which the first pass does not write). MT7977 (AdieSel byte 7) only.
 * [static][measured] */
#define MT7996_PHY_GSEL_SET1              1u
#define   MT7996_PHY_GSEL1_GRP_MT7977_BW320  { 0x11110000u, 0x00000000u }
#define   MT7996_PHY_GSEL1_STEP_MT7977_BW320 \
	{ 0x08000909u, 0x07060008u, 0x00050400u, 0x00000302u, 0x00000001u }

/*
 * IQ engine per-bin refinement V:e004a11e(first chain, end, P[0], step,
 * sp8f, P, mode) [static][measured]: runs after every step's IQ engine run
 * when P[0] != 0, mode 0x13 and bw 1..4. Bins s6 = 16 / 32 / 64 for sp8f =
 * 2 / 1 / 0 (40 / 80 / 160 and 320 MHz above 2.4 GHz), edge width s8 = 4 /
 * 8 / 16, levels s7 = 1 / 2 / 3.
 *  1. per chain: WM table T[bin] = 0x00001000 (1.0 + 0j) for all bins;
 *     IQBIN_ACC [0] = 0, [1] = 1; BN twin +0x84a8 [2] = 0, [1] = 1;
 *     TSSI_MEAS_CTL [27] = 1; for bins 0..s8+1 and s6-1-s8..s6-1: IQE_CFG0
 *     [15:8] = bin, [2] = 1, 5 us, T[bin] = RFINTF_IQBIN(chain), [2] = 0,
 *     1 us. The middle bins keep 1.0. Bins 2^t and s6 - 2^t (t =
 *     2..s7+1) are replaced by the rounded mean of their neighbours (each
 *     14-bit field separately); at 320 MHz the last level also copies the
 *     outer neighbour into the 6 bins next to it towards the middle.
 *  2. per chain: IQBIN_ACC [0] = 0, [1] = 1; BN twin +0x84a8 [2] = 0, [1] =
 *     1; TSSI_MEAS_CTL [27] = 1; for every bin: IQE_CFG0 [15:8] = bin,
 *     IQBIN_DATA = T[bin], IQE_CFG0 [2] = 1, IQBIN_ACC [0] = 1, 5 us, [0] =
 *     0, 5 us, IQE_CFG0 [2] = 0; then IQBIN_ACC [0], [1] = 0, IQE_CFG0 [2] =
 *     0, BN twin +0x84a8 [2], [1], [0] = 0, TSSI_MEAS_CTL [27] = 0; WF_COMM
 *     +0x3008 [4] pulsed; the IQE_CTRL table-fit sequence; reset pulse;
 *     chain 0's RXIQC_A word 0 [31] = a per-chain flag (0 observed); the 20
 *     words at RXIQC_FIT_CHAIN's live slot copied to WM RAM.
 *  3. every chain's live slot = its copy; IQE_CFG2 [3:2] = sp8f.
 * Access volume per step (oracle): 1406 at 40 MHz, 2334 at 80, 4190 at 160
 * on die 1 (4 chains); 1757 / 2917 / 5237 / 5237 at 40 / 80 / 160 / 320 MHz
 * on die 2 (5 chains); a BW320 call is about 108k accesses. The rest of a
 * step is mostly the IQ engine done poll, which also lengthens with
 * bandwidth.
 */
#define MT7996_PHY_IQBIN_BINS(sp8f)       (16u << (2u - (sp8f)))
#define MT7996_PHY_IQBIN_EDGE(sp8f)       (4u << (2u - (sp8f)))
#define MT7996_PHY_IQBIN_ONE              0x00001000u
#define MT7996_PHY_BN2_84A8(band)         MT7996_PHY_BN2(band, 0x84a8u)	/* twin; [2:0] */

/* WF_COMM +0xe0bc..+0xe0c8: "Mask Coeff" words printed by the vendor TM
 * (four words), in the DPD training engine's block (phy_cal.h): the
 * amplitude-bin table [inferred], written by some DPD steps only, then
 * restored. With P[6] (EPA, the stock case): 0x20202020, 0x21212122,
 * 0x22232324, 0x25262728 on MT7976 steps 6, 7 (group 0) and MT7977 steps 6,
 * 7 (group != 0). Without P[6]: the same on MT7976 step 3 (group 0) and
 * MT7977 steps 3, 4 (group != 0); 0x20202020, 0x20202121, 0x21212122,
 * 0x22222323 on MT7976 step 3 (group != 0). [static, measured] */
#define MT7996_PHY_MASK_COEFF(band, n)    MT7996_PHY_WFCOMM(band, 0xe0bcu + 4u * (n))

/*
 * DFS (PHYDFS_WF_COMM, ROM module 0x5f): band 0 at 0x830af000; band 1
 * resolves to 0x830bf000 (not 0x831af000) in the ROM; band 2 has no table.
 * [static]
 * DFS_5 +0x14: same layout as RDD_TOP_5 (DFS_TEST_PULSE_EN [31] ...).
 * DFS_16 +0x4c: [31:24] AGC_IB_TOTAL_GAIN (band in-band total gain),
 *   [8:6] WF_DFSCLK_SEL, [5] WF_DFS_EN, [4] DFS_RDD_EN, [3:0] DFS_CBW.
 * DFS_55 +0xe8: [0] DFS_RDD_LOGRST_B (active-low logic reset).
 */
#define MT7996_PHY_DFS_BASE(band)         ((band) ? 0x830bf000u : 0x830af000u)
#define MT7996_PHY_DFS5(band)             (MT7996_PHY_DFS_BASE(band) + 0x14u)
#define MT7996_PHY_DFS16(band)            (MT7996_PHY_DFS_BASE(band) + 0x4cu)
#define   MT7996_PHY_DFS16_AGC_IB_TOTAL_GAIN HW_GENMASK(31, 24)
#define   MT7996_PHY_DFS16_DFSCLK_SEL     HW_GENMASK(8, 6)
#define   MT7996_PHY_DFS16_DFS_EN         HW_BIT(5)
#define   MT7996_PHY_DFS16_RDD_EN         HW_BIT(4)
#define   MT7996_PHY_DFS16_CBW            HW_GENMASK(3, 0)
#define MT7996_PHY_DFS55(band)            (MT7996_PHY_DFS_BASE(band) + 0xe8u)
#define   MT7996_PHY_DFS55_RDD_LOGRST_B   HW_BIT(0)

/* Offset names, role unknown unless a header says more: rows of the per-band PHY init's
 * register tables (phy_init.h MT7996_PHY_INIT_REGTBL_*; "init", written whole on every
 * band, never read by the init) and of the channel-set PHY tables A / B (phy_chan.h
 * MT7996_CHSET_TBL_A_ROWS / _B_ROWS; "table A" / "table B"), and words the channel set
 * writes by field. [static][measured]
 * Sub-blocks: +0x0000..+0x0084 WFC_DSP (DSP-visible words); +0x2000..+0x2214
 * WFC_RDD (radar detectors; MT7996_PHY_RDD_*); +0x6000.. (no map entry); +0x7000..
 * WFC_AGC (band AGC words); +0xd214, +0xd418 WFC_DFE; +0xf038..+0xf044: band 0's
 * DFS_B0 (MT7996_PHY_DFS_BASE) as the init's absolute table names it; the writer
 * adds band << 20, so bands 1 and 2 get WF_COMM +0xf000 of their own band, not the
 * DFS block of band 1 at 0x830bf000. */
#define MT7996_PHY_WFCOMM_0000(band)       MT7996_PHY_WFCOMM(band, 0x0000u)	/* table B */
#define MT7996_PHY_WFCOMM_0004(band)       MT7996_PHY_WFCOMM(band, 0x0004u)	/* table B */
#define MT7996_PHY_WFCOMM_0044(band)       MT7996_PHY_WFCOMM(band, 0x0044u)	/* init */
#define MT7996_PHY_WFCOMM_0050(band)       MT7996_PHY_WFCOMM(band, 0x0050u)	/* table B */
#define MT7996_PHY_WFCOMM_0058(band)       MT7996_PHY_WFCOMM(band, 0x0058u)	/* init */
#define MT7996_PHY_WFCOMM_005C(band)       MT7996_PHY_WFCOMM(band, 0x005cu)	/* init */
#define MT7996_PHY_WFCOMM_0078(band)       MT7996_PHY_WFCOMM(band, 0x0078u)	/* init */
#define MT7996_PHY_WFCOMM_0084(band)       MT7996_PHY_WFCOMM(band, 0x0084u)	/* init */
#define MT7996_PHY_RDD_2000(band)          MT7996_PHY_WFCOMM(band, 0x2000u)	/* init */
#define MT7996_PHY_WFCOMM_2004(band)       MT7996_PHY_WFCOMM(band, 0x2004u)	/* table B */
#define MT7996_PHY_WFCOMM_2008(band)       MT7996_PHY_WFCOMM(band, 0x2008u)	/* table B */
#define MT7996_PHY_RDD_200C(band)          MT7996_PHY_WFCOMM(band, 0x200cu)	/* init */
#define MT7996_PHY_RDD_2010(band)          MT7996_PHY_WFCOMM(band, 0x2010u)	/* init */
#define MT7996_PHY_RDD_203C(band)          MT7996_PHY_WFCOMM(band, 0x203cu)	/* init */
#define MT7996_PHY_RDD_2040(band)          MT7996_PHY_WFCOMM(band, 0x2040u)	/* init */
#define MT7996_PHY_RDD_2044(band)          MT7996_PHY_WFCOMM(band, 0x2044u)	/* init */
#define MT7996_PHY_RDD_2048(band)          MT7996_PHY_WFCOMM(band, 0x2048u)	/* init */
#define MT7996_PHY_WFCOMM_21E0(band)       MT7996_PHY_WFCOMM(band, 0x21e0u)	/* table B */
#define MT7996_PHY_RDD_21E4(band)          MT7996_PHY_WFCOMM(band, 0x21e4u)	/* init */
#define MT7996_PHY_RDD_2214(band)          MT7996_PHY_WFCOMM(band, 0x2214u)	/* init */
#define MT7996_PHY_WFCOMM_6020(band)       MT7996_PHY_WFCOMM(band, 0x6020u)	/* init */
#define MT7996_PHY_WFCOMM_6024(band)       MT7996_PHY_WFCOMM(band, 0x6024u)	/* init */
#define MT7996_PHY_WFCOMM_604C(band)       MT7996_PHY_WFCOMM(band, 0x604cu)	/* init */
#define MT7996_PHY_WFCOMM_6070(band)       MT7996_PHY_WFCOMM(band, 0x6070u)	/* init */
#define MT7996_PHY_WFCOMM_6108(band)       MT7996_PHY_WFCOMM(band, 0x6108u)	/* table B */
#define MT7996_PHY_WFCOMM_610C(band)       MT7996_PHY_WFCOMM(band, 0x610cu)	/* init */
#define MT7996_PHY_WFCOMM_6110(band)       MT7996_PHY_WFCOMM(band, 0x6110u)	/* init */
#define MT7996_PHY_WFCOMM_6114(band)       MT7996_PHY_WFCOMM(band, 0x6114u)	/* init */
#define MT7996_PHY_WFCOMM_6118(band)       MT7996_PHY_WFCOMM(band, 0x6118u)	/* init */
#define MT7996_PHY_WFCOMM_611C(band)       MT7996_PHY_WFCOMM(band, 0x611cu)	/* init */
#define MT7996_PHY_WFCOMM_6124(band)       MT7996_PHY_WFCOMM(band, 0x6124u)	/* table B */
#define MT7996_PHY_WFCOMM_6128(band)       MT7996_PHY_WFCOMM(band, 0x6128u)	/* init */
#define MT7996_PHY_WFCOMM_612C(band)       MT7996_PHY_WFCOMM(band, 0x612cu)	/* init */
#define MT7996_PHY_WFCOMM_6204(band)       MT7996_PHY_WFCOMM(band, 0x6204u)	/* init */
#define MT7996_PHY_WFCOMM_6804(band)       MT7996_PHY_WFCOMM(band, 0x6804u)	/* init */
#define MT7996_PHY_WFCOMM_6808(band)       MT7996_PHY_WFCOMM(band, 0x6808u)	/* init */
#define MT7996_PHY_WFCOMM_6814(band)       MT7996_PHY_WFCOMM(band, 0x6814u)	/* init */
#define MT7996_PHY_WFCOMM_6828(band)       MT7996_PHY_WFCOMM(band, 0x6828u)	/* init */
#define MT7996_PHY_WFCOMM_68A0(band)       MT7996_PHY_WFCOMM(band, 0x68a0u)	/* init */
#define MT7996_PHY_WFCOMM_68B8(band)       MT7996_PHY_WFCOMM(band, 0x68b8u)	/* init */
#define MT7996_PHY_WFCOMM_701C(band)       MT7996_PHY_WFCOMM(band, 0x701cu)	/* init */
#define MT7996_PHY_WFCOMM_702C(band)       MT7996_PHY_WFCOMM(band, 0x702cu)	/* table B */
#define MT7996_PHY_WFCOMM_7064(band)       MT7996_PHY_WFCOMM(band, 0x7064u)	/* table B */
#define MT7996_PHY_WFCOMM_7068(band)       MT7996_PHY_WFCOMM(band, 0x7068u)	/* init */
#define MT7996_PHY_WFCOMM_706C(band)       MT7996_PHY_WFCOMM(band, 0x706cu)	/* init */
#define MT7996_PHY_WFCOMM_7094(band)       MT7996_PHY_WFCOMM(band, 0x7094u)	/* table B */
#define MT7996_PHY_WFCOMM_70BC(band)       MT7996_PHY_WFCOMM(band, 0x70bcu)	/* table B */
#define MT7996_PHY_WFCOMM_70C4(band)       MT7996_PHY_WFCOMM(band, 0x70c4u)	/* init */
#define MT7996_PHY_WFCOMM_70F4(band)       MT7996_PHY_WFCOMM(band, 0x70f4u)	/* init */
#define MT7996_PHY_WFCOMM_7100(band)       MT7996_PHY_WFCOMM(band, 0x7100u)	/* init */
#define MT7996_PHY_WFCOMM_7130(band)       MT7996_PHY_WFCOMM(band, 0x7130u)	/* init */
#define MT7996_PHY_WFCOMM_7134(band)       MT7996_PHY_WFCOMM(band, 0x7134u)	/* init */
#define MT7996_PHY_WFCOMM_7138(band)       MT7996_PHY_WFCOMM(band, 0x7138u)	/* init */
#define MT7996_PHY_WFCOMM_7158(band)       MT7996_PHY_WFCOMM(band, 0x7158u)	/* init */
#define MT7996_PHY_WFCOMM_7160(band)       MT7996_PHY_WFCOMM(band, 0x7160u)	/* table B */
#define MT7996_PHY_WFCOMM_71AC(band)       MT7996_PHY_WFCOMM(band, 0x71acu)	/* init */
#define MT7996_PHY_WFCOMM_71B4(band)       MT7996_PHY_WFCOMM(band, 0x71b4u)	/* init */
#define MT7996_PHY_WFCOMM_7258(band)       MT7996_PHY_WFCOMM(band, 0x7258u)	/* init */
#define MT7996_PHY_WFCOMM_725C(band)       MT7996_PHY_WFCOMM(band, 0x725cu)	/* init */
#define MT7996_PHY_WFCOMM_726C(band)       MT7996_PHY_WFCOMM(band, 0x726cu)	/* table B */
#define MT7996_PHY_WFCOMM_7278(band)       MT7996_PHY_WFCOMM(band, 0x7278u)	/* table B */
#define MT7996_PHY_WFCOMM_72AC(band)       MT7996_PHY_WFCOMM(band, 0x72acu)	/* init */
#define MT7996_PHY_WFCOMM_7368(band)       MT7996_PHY_WFCOMM(band, 0x7368u)	/* init */
#define MT7996_PHY_WFCOMM_736C(band)       MT7996_PHY_WFCOMM(band, 0x736cu)	/* init */
#define MT7996_PHY_WFCOMM_7370(band)       MT7996_PHY_WFCOMM(band, 0x7370u)	/* init */
#define MT7996_PHY_WFCOMM_7374(band)       MT7996_PHY_WFCOMM(band, 0x7374u)	/* init */
#define MT7996_PHY_WFCOMM_7378(band)       MT7996_PHY_WFCOMM(band, 0x7378u)	/* init */
#define MT7996_PHY_WFCOMM_737C(band)       MT7996_PHY_WFCOMM(band, 0x737cu)	/* init */
#define MT7996_PHY_WFCOMM_7380(band)       MT7996_PHY_WFCOMM(band, 0x7380u)	/* init */
#define MT7996_PHY_WFCOMM_7444(band)       MT7996_PHY_WFCOMM(band, 0x7444u)	/* init */
#define MT7996_PHY_WFCOMM_7448(band)       MT7996_PHY_WFCOMM(band, 0x7448u)	/* table B */
#define MT7996_PHY_WFCOMM_7450(band)       MT7996_PHY_WFCOMM(band, 0x7450u)	/* init */
#define MT7996_PHY_WFCOMM_7454(band)       MT7996_PHY_WFCOMM(band, 0x7454u)	/* init */
#define MT7996_PHY_WFCOMM_7474(band)       MT7996_PHY_WFCOMM(band, 0x7474u)	/* init */
#define MT7996_PHY_WFCOMM_7500(band)       MT7996_PHY_WFCOMM(band, 0x7500u)	/* init */
#define MT7996_PHY_WFCOMM_7504(band)       MT7996_PHY_WFCOMM(band, 0x7504u)	/* init */
#define MT7996_PHY_WFCOMM_750C(band)       MT7996_PHY_WFCOMM(band, 0x750cu)	/* init */
#define MT7996_PHY_WFCOMM_752C(band)       MT7996_PHY_WFCOMM(band, 0x752cu)	/* init */
#define MT7996_PHY_WFCOMM_7600(band)       MT7996_PHY_WFCOMM(band, 0x7600u)	/* init */
#define MT7996_PHY_WFCOMM_7608(band)       MT7996_PHY_WFCOMM(band, 0x7608u)	/* init */
#define MT7996_PHY_WFCOMM_7618(band)       MT7996_PHY_WFCOMM(band, 0x7618u)	/* init */
#define MT7996_PHY_WFCOMM_7630(band)       MT7996_PHY_WFCOMM(band, 0x7630u)	/* init */
#define MT7996_PHY_WFCOMM_7634(band)       MT7996_PHY_WFCOMM(band, 0x7634u)	/* init */
#define MT7996_PHY_WFCOMM_7670(band)       MT7996_PHY_WFCOMM(band, 0x7670u)	/* init */
#define MT7996_PHY_WFCOMM_7674(band)       MT7996_PHY_WFCOMM(band, 0x7674u)	/* init */
#define MT7996_PHY_WFCOMM_7678(band)       MT7996_PHY_WFCOMM(band, 0x7678u)	/* init */
#define MT7996_PHY_WFCOMM_767C(band)       MT7996_PHY_WFCOMM(band, 0x767cu)	/* init */
#define MT7996_PHY_WFCOMM_7680(band)       MT7996_PHY_WFCOMM(band, 0x7680u)	/* init */
#define MT7996_PHY_WFCOMM_768C(band)       MT7996_PHY_WFCOMM(band, 0x768cu)	/* init */
#define MT7996_PHY_WFCOMM_7690(band)       MT7996_PHY_WFCOMM(band, 0x7690u)	/* init */
#define MT7996_PHY_WFCOMM_7694(band)       MT7996_PHY_WFCOMM(band, 0x7694u)	/* init */
#define MT7996_PHY_WFCOMM_76A4(band)       MT7996_PHY_WFCOMM(band, 0x76a4u)	/* init */
#define MT7996_PHY_WFCOMM_76B4(band)       MT7996_PHY_WFCOMM(band, 0x76b4u)	/* init */
#define MT7996_PHY_WFCOMM_7778(band)       MT7996_PHY_WFCOMM(band, 0x7778u)	/* init */
#define MT7996_PHY_WFCOMM_777C(band)       MT7996_PHY_WFCOMM(band, 0x777cu)	/* init */
#define MT7996_PHY_WFCOMM_7798(band)       MT7996_PHY_WFCOMM(band, 0x7798u)	/* init */
#define MT7996_PHY_WFCOMM_77A4(band)       MT7996_PHY_WFCOMM(band, 0x77a4u)	/* init */
#define MT7996_PHY_WFCOMM_77A8(band)       MT7996_PHY_WFCOMM(band, 0x77a8u)	/* init */
#define MT7996_PHY_WFCOMM_77AC(band)       MT7996_PHY_WFCOMM(band, 0x77acu)	/* init */
#define MT7996_PHY_WFCOMM_77B0(band)       MT7996_PHY_WFCOMM(band, 0x77b0u)	/* init */
#define MT7996_PHY_WFCOMM_77B4(band)       MT7996_PHY_WFCOMM(band, 0x77b4u)	/* init */
#define MT7996_PHY_WFCOMM_77B8(band)       MT7996_PHY_WFCOMM(band, 0x77b8u)	/* init */
#define MT7996_PHY_WFCOMM_77D8(band)       MT7996_PHY_WFCOMM(band, 0x77d8u)	/* init */
#define MT7996_PHY_WFCOMM_77DC(band)       MT7996_PHY_WFCOMM(band, 0x77dcu)	/* init */
#define MT7996_PHY_WFCOMM_77E0(band)       MT7996_PHY_WFCOMM(band, 0x77e0u)	/* init */
#define MT7996_PHY_WFCOMM_77E8(band)       MT7996_PHY_WFCOMM(band, 0x77e8u)	/* init */
#define MT7996_PHY_WFCOMM_77EC(band)       MT7996_PHY_WFCOMM(band, 0x77ecu)	/* init */
#define MT7996_PHY_WFCOMM_7820(band)       MT7996_PHY_WFCOMM(band, 0x7820u)	/* init */
#define MT7996_PHY_WFCOMM_782C(band)       MT7996_PHY_WFCOMM(band, 0x782cu)	/* init */
#define MT7996_PHY_WFCOMM_7848(band)       MT7996_PHY_WFCOMM(band, 0x7848u)	/* init */
#define MT7996_PHY_WFCOMM_784C(band)       MT7996_PHY_WFCOMM(band, 0x784cu)	/* init */
#define MT7996_PHY_WFCOMM_7858(band)       MT7996_PHY_WFCOMM(band, 0x7858u)	/* init */
#define MT7996_PHY_WFCOMM_7A84(band)       MT7996_PHY_WFCOMM(band, 0x7a84u)	/* init */
#define MT7996_PHY_WFCOMM_7A88(band)       MT7996_PHY_WFCOMM(band, 0x7a88u)	/* init */
#define MT7996_PHY_WFCOMM_7A8C(band)       MT7996_PHY_WFCOMM(band, 0x7a8cu)	/* init */
#define MT7996_PHY_WFCOMM_7AA4(band)       MT7996_PHY_WFCOMM(band, 0x7aa4u)	/* init */
#define MT7996_PHY_WFCOMM_7AA8(band)       MT7996_PHY_WFCOMM(band, 0x7aa8u)	/* init */
#define MT7996_PHY_WFCOMM_7B28(band)       MT7996_PHY_WFCOMM(band, 0x7b28u)	/* init */
#define MT7996_PHY_WFCOMM_7B44(band)       MT7996_PHY_WFCOMM(band, 0x7b44u)	/* init */
#define MT7996_PHY_WFCOMM_7B8C(band)       MT7996_PHY_WFCOMM(band, 0x7b8cu)	/* init */
#define MT7996_PHY_WFCOMM_7B90(band)       MT7996_PHY_WFCOMM(band, 0x7b90u)	/* init */
#define MT7996_PHY_WFCOMM_7BB8(band)       MT7996_PHY_WFCOMM(band, 0x7bb8u)	/* init */
#define MT7996_PHY_WFCOMM_7BBC(band)       MT7996_PHY_WFCOMM(band, 0x7bbcu)	/* init */
#define MT7996_PHY_WFCOMM_7C4C(band)       MT7996_PHY_WFCOMM(band, 0x7c4cu)	/* init */
#define MT7996_PHY_WFCOMM_7C64(band)       MT7996_PHY_WFCOMM(band, 0x7c64u)	/* init */
#define MT7996_PHY_WFCOMM_7C68(band)       MT7996_PHY_WFCOMM(band, 0x7c68u)	/* init */
#define MT7996_PHY_WFCOMM_7CC4(band)       MT7996_PHY_WFCOMM(band, 0x7cc4u)	/* init */
#define MT7996_PHY_WFCOMM_7CE4(band)       MT7996_PHY_WFCOMM(band, 0x7ce4u)	/* init */
#define MT7996_PHY_WFCOMM_7CE8(band)       MT7996_PHY_WFCOMM(band, 0x7ce8u)	/* init */
#define MT7996_PHY_WFCOMM_B000(band)       MT7996_PHY_WFCOMM(band, 0xb000u)	/* table A */
#define MT7996_PHY_WFCOMM_B538(band)       MT7996_PHY_WFCOMM(band, 0xb538u)	/* table A */
#define MT7996_PHY_WFCOMM_B59C(band)       MT7996_PHY_WFCOMM(band, 0xb59cu)	/* table A */

/* +0xc058: with E2 parts table A's explicit fields set [16], then [20]. Role
 * unknown. [static] */
#define MT7996_PHY_WFCOMM_C058(band)       MT7996_PHY_WFCOMM(band, 0xc058u)	/* table A */
#define   MT7996_PHY_WFCOMM_C058_B16    HW_BIT(16)
#define   MT7996_PHY_WFCOMM_C058_B20    HW_BIT(20)

#define MT7996_PHY_WFCOMM_D214(band)       MT7996_PHY_WFCOMM(band, 0xd214u)	/* init */

/* WF_COMM +0xd24c [15]: written by V:e00ddd0e on bands 1 and 2 only (the band != 0 tests at
 * V:e00de0a2 / V:e00de164 / V:e00de4dc): 1 at BW80, 0 at BW20 / 40 / 160 / 320; WM PHY
 * preparation clears it on bands 1 and 2 (phy_cal.h). Stock 0, unchanged. Role unknown.
 * [static][measured] */
#define MT7996_PHY_WFCOMM_D24C(band)       MT7996_PHY_WFCOMM(band, 0xd24cu)
#define   MT7996_PHY_WFCOMM_D24C_B15      HW_BIT(15)

#define MT7996_PHY_WFCOMM_D418(band)       MT7996_PHY_WFCOMM(band, 0xd418u)	/* init */
#define MT7996_PHY_WFCOMM_D4EC(band)       MT7996_PHY_WFCOMM(band, 0xd4ecu)	/* table A */
#define MT7996_PHY_WFCOMM_D4F4(band)       MT7996_PHY_WFCOMM(band, 0xd4f4u)	/* table A */
#define MT7996_PHY_WFCOMM_D4F8(band)       MT7996_PHY_WFCOMM(band, 0xd4f8u)	/* table A */
#define MT7996_PHY_WFCOMM_F00C(band)       MT7996_PHY_WFCOMM(band, 0xf00cu)	/* table B */
#define MT7996_PHY_WFCOMM_F038(band)       MT7996_PHY_WFCOMM(band, 0xf038u)	/* init */
#define MT7996_PHY_WFCOMM_F03C(band)       MT7996_PHY_WFCOMM(band, 0xf03cu)	/* init */
#define MT7996_PHY_WFCOMM_F044(band)       MT7996_PHY_WFCOMM(band, 0xf044u)	/* init */

/* ========================================================================
 * Per-band PHY init (calibration init V:e00d6c3e, before item 0; phy_cal.h
 * "Calibration init"), for band b = 0, 1, 2 when the band's EEPROM field is
 * nonzero (EEPROM 0x190 [2:0], 0x190 [5:3], 0x191 [2:0]; V:e00e02be);
 * B = b << 20. Decoded against MediaTek's cold stock load (14,183 records,
 * regenerated record for record). In order: [static][measured]
 *  1. V:e00de622 (D-die, straight-line): WF_COMM +0xd430 (phy_cal.h
 *     MT7996_PHY_DPD_GAIN_ERR) = 0x395832a4; +0xd208 [1] = 1; RFINTF +0x28c
 *     on chains 0..4 = 0x44664447; band 0 only: DFE_BN_TX 0x8308e334 [7:0]
 *     = 0x14 and MT7996_PHY_DFE_E2A4 / _E2C0(0, 0..2) (phy_cal.h); bands 1
 *     and 2 only: WF_COMM +0xd234 [8] = 1, +0xd230 [5:4] = 1, +0xd240
 *     [14:0] = 0x11d4; then on all bands: DFE_BN_TX +0xe330, CHAIN_87AC
 *     [31:30] = 2 (chains 0..3), the gain-index selectors MT7996_PHY_GSEL_*
 *     (values by A-die type), WF_COMM +0xc0fc / +0xc0a4 / +0xd4f0 / +0x38e4
 *     / +0x38f0 / +0x3824 / +0x38b4 / +0x38c4 / +0x3844 / +0xd480 / +0xd4cc,
 *     BN +0x8240, RFINTF +0x268 / +0x26c (chains 0..4), BN +0x8590 /
 *     +0x85a0, WF_COMM +0x3034, BN +0x8e30 and the band-2 coefficient words
 *     +0x8e60..+0x8e74, BN_PHYINTF +0x2aa0..+0x2b10, WF_COMM +0x303c, BN
 *     +0x0044 / +0x8234 / +0x10a4 (and twin) / +0x6094 (and twin), the
 *     revision branch (MT7996_CBTOP_HW_REV != 0x8a00: CHAIN_D0D0 [0] = 1 on
 *     chains 0..3; E1: WF_COMM +0xb580 [24:23] = 1, BN +0x8590 / +0x85a0
 *     [15:14] = 0, BN +0x8e30 [31] = 0), WF_COMM +0xc104 / +0xc0fc / +0x303c
 *     / +0x3040 / +0x3044;
 *  2. V:e00dbfe6, the TOP init: A-die TOP and D-die +0xd480 / +0xd49c /
 *     +0xd4a0, MT7976 or MT7977 branch by the AdieSel byte (adie.h "TOP
 *     init");
 *  3. V:e00de5f6: WFSYS_ON LPCR_DIS(0), LPCR_DIS(1), LPCR_EN(0), LPCR_EN(1)
 *     = 0x10 (conn.h; fixed addresses, on every band's pass);
 *  4. per-chain fills (chains 0..3): MT7996_PHY_CHAIN_9000 = 0x1001,
 *     _9500, _9A00, _B000, _B500 = 0;
 *  5. CHAIN_D37C [4:0] = 0, CHAIN_D3B8 [19:0] = 0x01201, CHAIN_D3BC [19:0]
 *     = 0x01212 (chains 0..3);
 *  6. V:e00d98ac, the MemDPD seed (phy_cal.h MT7996_PHY_FW_D7D8; nothing
 *     written here, both flags 0);
 *  7. V:e00d8f80, the register tables (phy_map.h MT7996_PHY_REGTBL_*): 45
 *     per-chain rows x chains 0..3, then 312 absolute rows, + B.
 * The same values on all three bands except the band-0 / band-1, 2 branch
 * (1.), the A-die type (MT7996_PHY_GSEL_*, the TOP init branch) and
 * read-back bits. For a receiver's own cold init [inferred]: the RX blocks'
 * table rows (CSD 55, WFC_AGC 82, BN_RXTD 50, RXFE and +0xf200 27,
 * CHAIN_E000 30, WFC_DFE 2), the gain-index selectors, the TOP init,
 * BN_PHYINTF / PHYCTRL, CHAIN_87AC and the WFSYS_ON PLL clock-gate pulse,
 * with rows of unknown role replicated as constants; +0xd430, the TSSI /
 * BF / TX rows and the MemDPD seed are TX-side.
 * ======================================================================== */

/* ========================================================================
 * Band AFE control (CONN AFE). ADDRESS SPACE: connectivity bus
 * 0x7c043000 + band * 0x1000 (host 0x18043000 + band * 0x1000). The ADC/DAC
 * analog interface of each band, directly in front of the RX front end
 * above. +0x08 (ADC stop) is defined in conn.h (MT7996_CONN_AFE_CTL08).
 * [measured on band 1 unless stated]
 *  +0x00: [16] forces a constant ADC code; [20] adds DC and a +120 MHz line.
 *  +0x08: [24] stops the band's ADC; rest value 0x00006666 (conn.h).
 *  +0x14: [6:3] = 0xf disconnects the ADC input. Calibration writes: [6:3]
 *         = 0xf and [18] (D-die AFE/RXFE cal, disabled by default), [14]
 *         (V:e004b4b4, mode 0xf, the code after the T1[23] body: not the
 *         TTG RX calibration, phy_cal.h). [static for the cal writes]
 *  +0x20: read-only.
 *  +0x2c: PLL_03, [1] MSB_EN. [static: mt76]
 * HAZARD: +0x08[24] survives a radio reload; the stream does not start until
 * the word is rewritten (0x00006666) and the radio reloaded. [measured]
 * ======================================================================== */

#define MT7996_PHY_AFE(band, off)         (0x7c043000u + ((band) << 12) + (off))
#define MT7996_PHY_AFE_CTL0(band)         MT7996_PHY_AFE(band, 0x00u)
#define   MT7996_PHY_AFE_CTL0_CONST_CODE  HW_BIT(16)
#define   MT7996_PHY_AFE_CTL0_DC_TEST     HW_BIT(20)
#define MT7996_PHY_AFE_CTL5(band)         MT7996_PHY_AFE(band, 0x14u)
#define   MT7996_PHY_AFE_CTL5_IN_DISC     HW_GENMASK(6, 3)	/* 0xf = input disconnected */
#define   MT7996_PHY_AFE_CTL5_B14         HW_BIT(14)
#define   MT7996_PHY_AFE_CTL5_B18         HW_BIT(18)
#define MT7996_PHY_AFE_STAT(band)         MT7996_PHY_AFE(band, 0x20u)
#define MT7996_PHY_AFE_PLL03(band)        MT7996_PHY_AFE(band, 0x2cu)
#define   MT7996_PHY_AFE_PLL03_MSB_EN     HW_BIT(1)

/* ========================================================================
 * The PHY seen from the capture path [measured unless stated]
 *
 * - Sample rate at the capture taps = 2 x channel bandwidth (40 Msps on a
 *   20 MHz channel ... 640 Msps on 320 MHz). Per band: band 0 2 x 320 Msps
 *   (160 MHz: our channel set, MediaTek's 2.4 GHz tables end at 40 MHz),
 *   band 1 2 x 320 Msps (160 MHz), band 2 1 x 640 Msps (320 MHz). Samples
 *   are 14-bit signed I/Q.
 * - The capture configuration writes no ADC clock: on a 20 MHz channel a
 *   wider capture-bandwidth setting only repeats samples (50/75/87.5 %).
 *   The channel bandwidth programmed by the channel command sets the rate.
 * - RX gain forces (RXGAIN sets A/B), LO, analog bandwidth (A-die) and RC
 *   trim (A-die) all act on the captured stream mid-capture.
 * - Between captures under AGC, path amplitude ratio and inter-path phase
 *   flip between two states; force the gain before comparing captures.
 * - ICAP source 6 ("AFE") never strobes in any firmware state, including
 *   with BN_CLK[10], [25], [29], RXFE01 AFIFO_RD_EN / DBG_SEL_RX and nine
 *   sampled values of the capture block's internal debug select; sources 0
 *   and 7 strobe under a host arm.
 * - While a capture is armed (EN set), each capture-window page reads back
 *   as its last-fetched 8 bytes repeated: a frozen bus view, not memory
 *   content and not IQ. After EN clears the window holds the capture.
 * - Chain 4 of band 0 (monitor receiver) has not been captured: its
 *   capture domain streams zeros with the monitor, RDD2 and all background
 *   enables above set.
 * ======================================================================== */

#endif /* HW_MT7996_PHY_H */
