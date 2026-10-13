/* SPDX-License-Identifier: MIT */
/*
 * MT7996 capture path set-up: what MediaTek's SPECTRUM capture arm (UNI command 0x30 tag 0,
 * spectrum_cmd.h) leaves programmed on a band, decoded from the vendor TESTMODE WM's handler
 * V:e00aba5e (start V:e00ab708, per-instance loop V:e00ab5fe, PHY/DFE writes V:e00ab6ce): the
 * stock request and node record. The registers are named in phy.h (MT7996_PHY_CAPTRIG*,
 * MT7996_PHY_WFCOMM_3008_CAP_CLR*), dfe.h, icap.h and cap_route.h. Generated from MediaTek's WM
 * image by the capture port's gen_header.py (research tree, radio-port/capture/); do not edit by
 * hand.
 *
 * ADDRESS SPACE: WF bus as the WM sees it (phy.h, dfe.h); band b at + b << 20.
 *
 * The arm, per band (band index = PHY index = b), in MediaTek's order [static]:
 *  1. per ICAP instance 0..2: the capture clear V:e012e30e(1) (below), ICAPCR0 SW_RST_B pulse,
 *     packer (ICAP2 64->128 only), route (cap_route.h type 1), EN / SRC_SEL / RING_BUF, ICAPCR8
 *     (raw word 0 or AFE_SEL, then WF_BANK_SEL), ADDR0 = [0, 0xffc], stop cycle (icap.h);
 *  2. CAPTURE on ICAP0..2;
 *  3. BN_CLK0 / BN_CLK1 |= CAPTURE (phy.h), V:e012e504 / V:e012e522;
 *  4. the capture configure V:e012e30e(0): DFE CTRL |= EN, DFE +0xc00 [19:18] (set for a
 *     domain-6 node, else cleared), WF_COMM +0x3008 [22:21] = 0, CAPTRIG_CFG [7:0] = 0x10,
 *     DFE MODE [7:0] = the node record's activation code (the node's config field [19:12] when
 *     no record matches), DFE LANE = the lane word (below);
 *  5. the node's DFE mux fields V:e012e5d2 (dfe.h "Capture nodes"): MUX SEL0, RATE0 (the
 *     record's rate byte for u4BW), ACT0, each one read-modify-write;
 *  6. the capture trigger V:e012e540 (phy.h MT7996_PHY_CAPTRIG).
 * The capture clear V:e012e30e(1): CAPTRIG_CFG [7:0] = 0, WF_COMM +0x3008 |= [21] then |= [22],
 * DFE MUX = 0, DFE MUX2 = 0. The retire (enable 0) clears ICAPCR0 CAPTURE (ICAP0, or all three
 * when PHY index = 1) and the route grant (cap_route.h +0x04) and touches nothing above. Around both, the
 * vendor TM's manual-gain recipe V:e012e998 saves and restores BN +0x8200 / +0x8d60 / +0x8550
 * (phy.h) and pulses LMAC ARB_SCR [9]; it is not capture-path set-up.
 *
 * Confidence tags: [measured] verified on hardware, [static] read from firmware code or tables,
 * [inferred].
 */
#ifndef HW_MT7996_CAPTURE_SETUP_H
#define HW_MT7996_CAPTURE_SETUP_H

#include "../hw_bits.h"
#include "dfe.h"

/* ------------------------------------------------------------------------ */
/* Lane word (DFE LANE, dfe.h) by u4BW, node and PHY index [static]:       */
/* u4BW 5 with a node of category 2..6 takes lane table 1, of category       */
/* 13..16 table 2; u4BW 2 with category 14..17 and u4BW 4 with category 0 or */
/* 14..16 take table 1; every other case writes MT7996_DFE_CAP_LANE_FW_DEFAULT */
/* (after a read whose value is not used). Table 1 by the node's config      */
/* field: 4 / 19 -> MT7996_DFE_CAP_LANE_640M, 9 / 24 -> 0x0b030a02, 36 ->     */
/* 0x0c040c04; table 2: MT7996_DFE_CAP_LANE_FW_PATH(p) for config p / p + 15  */
/* (p = 0..3), 36 -> p = 4. The stock node (category 2, config 4) therefore  */
/* takes LANE_640M at u4BW 5 and the default otherwise.                      */
/* ------------------------------------------------------------------------ */

/* The prime's request (host/w1700k/capture.py icap_param): node MT7996_DFE_NODE_STOCK, trigger
 * event 0, source 0, ring 0, stop cycle 0. [measured] */
#define MT7996_CAPSETUP_NODE              MT7996_DFE_NODE_STOCK
#define MT7996_CAPSETUP_TRIG_EVENT        0u

/* MediaTek's capture-node record of MT7996_DFE_NODE_STOCK (dfe.h): WM table 0xe015b104
 * (53 records x 16 bytes, keys unique), record at 0xe015b1e4. Columns: SEL, record +0x04 /
 * +0x05 for PHY index != 1 / == 1; the rate byte by u4BW 0..5, record +0x06, +0x07, +0x08,
 * +0x09 (u4BW 3 and 4), +0x0a; the activation code, record +0x0b / +0x0c for PHY index != 1 /
 * == 1 (dfe.h MT7996_DFE_MODE_*). Entries X(column, value). [static][measured] */
#define MT7996_CAPNODE_STOCK_KEY        0x00204000u
#define MT7996_CAPNODE_STOCK_SEL(X) \
	X(0, 0x14u) X(1, 0x14u)
#define MT7996_CAPNODE_STOCK_RATE(X) \
	X(0, 3u) X(1, 2u) X(2, 1u) X(3, 0u) X(4, 0u) X(5, 0u)
#define MT7996_CAPNODE_STOCK_ACT(X) \
	X(0, 0x11u) X(1, 0x11u)

#endif /* HW_MT7996_CAPTURE_SETUP_H */
