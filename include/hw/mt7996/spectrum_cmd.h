/* SPDX-License-Identifier: MIT */
/*
 * MT7996 SPECTRUM firmware command (UNI command 0x30): the firmware's capture
 * request, status query and dump. On a TESTMODE WM it programs the whole
 * capture path (dfe.h, icap.h, cap_route.h); it is used here to "prime" that
 * path once, after which the host or an MCU program captures directly.
 *
 * ADDRESS SPACE: none; a wire format. A command body is sent on the WM
 * command path as UNI command id 0x30; replies arrive as UNI event id 0x30.
 * All fields are little-endian. Field names follow the mt76 test-mode
 * structure (tm_icap_param); the vendor block of the same layout is the
 * SET_SPECTRUM parameter structure.                                  [static]
 *
 * Body (command and event alike):
 *   +0  4 bytes reserved (UNI prefix)
 *   +4  u16 tag
 *   +6  u16 length: TLV header (4) + payload
 *   +8  payload
 *
 * Only the SPECTRUM command captures on the stock firmware. The TESTMODE_CTRL
 * (UNI 0x46) RF-test ICAP functions (0x0b set, 0x0c status, 0x11 raw data)
 * are stubs returning 0xc00000bb in the stock TESTMODE WM; the stock release
 * WM has no 0x46 handler.                                            [static]
 *
 * Confidence tags: [measured] verified on hardware, [static] read from
 * firmware code or host sources, [inferred].
 */
#ifndef HW_MT7996_SPECTRUM_CMD_H
#define HW_MT7996_SPECTRUM_CMD_H

#include "../hw_bits.h"

#define MT7996_SPECTRUM_CID		0x30u	/* UNI command id (mcu_cmd.h MCU_UNI_CMD_SPECTRUM) */
#define MT7996_SPECTRUM_EID		0x30u	/* UNI event id */

#define MT7996_SPECTRUM_BODY_TAG	4u	/* u16 */
#define MT7996_SPECTRUM_BODY_LEN	6u	/* u16, includes the 4-byte TLV header */
#define MT7996_SPECTRUM_BODY_DATA	8u
#define MT7996_SPECTRUM_TLV_LEN(payload) (4u + (unsigned int)(payload))

/* Command tags.                                                    [measured] */
#define MT7996_SPECTRUM_TAG_CAPTURE	0u	/* payload: parameter block */
#define MT7996_SPECTRUM_TAG_STATUS	1u	/* no payload */
#define MT7996_SPECTRUM_TAG_DUMP	2u	/* payload: dump parameters */

/* ------------------------------------------------------------------------ */
/* Tag 0 parameter block: 22 u32 words (88 bytes, TLV length 92). The stock */
/* TESTMODE WM requires TLV length >= 84.                          [measured] */
/* ------------------------------------------------------------------------ */
#define MT7996_SPECTRUM_PARAM_WORDS	22u
#define MT7996_SPECTRUM_PARAM_BYTES	88u
#define MT7996_SPECTRUM_PARAM_TLV_LEN	92u
#define MT7996_SPECTRUM_PARAM_TLV_MIN	84u
/* Byte offset of word w within the payload, and within the whole body.      */
#define MT7996_SPECTRUM_PARAM_OFS(w)	(4u * (unsigned int)(w))
#define MT7996_SPECTRUM_BODY_OFS(w)	(MT7996_SPECTRUM_BODY_DATA + 4u * (unsigned int)(w))

/* 1 = arm: program and start the capture. 0 = retire: release the route
 * (cap_route.h +0x04 = 0) and clear ICAPCR0 CAPTURE (ICAP0, all three
 * instances when PHY index = 1), inside the vendor test-mode gain recipe;
 * nothing of the DFE / PHY set-up.                        [measured] [static] */
#define MT7996_SPECTRUM_W_ENABLE	0u
/* Ring mode, to ICAPCR0.RING_BUF.                                    [static] */
#define MT7996_SPECTRUM_W_RING		1u
#define MT7996_SPECTRUM_W_TRIGGER_EVENT	2u
/* Capture node key (dfe.h): selects tap, rate byte and activation. 0x00204000
 * is the stock node.                                      [measured] [static] */
#define MT7996_SPECTRUM_W_NODE		3u
/* Capture length in samples. 2048 is the measured value; with it the stock
 * arm sets ADDR0 = [0, 0xffc], the whole 16 KiB window.   [measured] */
#define MT7996_SPECTRUM_W_LENGTH	4u
/* Stop cycle. The firmware writes it >> 1 (>> 2 in four-samples-per-beat
 * modes) to ICAPCR5; it does not bound the landing (icap.h).        [static] */
#define MT7996_SPECTRUM_W_CYCLE		5u
#define MT7996_SPECTRUM_W_MAC_TRIGGER	6u
/* Destination address words: never seen in ICAP0's address registers.
 *                                                                  [measured] */
#define MT7996_SPECTRUM_W_ADDR_LSB	7u
#define MT7996_SPECTRUM_W_ADDR_MSB	8u
/* Band index. Used for ICAPCR8 only by the unreachable route type 2; set it
 * equal to PHY_IDX.                                        [static] [measured] */
#define MT7996_SPECTRUM_W_BAND_IDX	9u
/* u4BW: picks the node record's rate byte for the DFE rate field (dfe.h
 * CAP_MUX_RATE0). Never range-checked. With node 0x00204000 the rate bytes
 * are 3, 2, 1, 0, 0, 0; u4BW 5 additionally writes the lane mux 0x09010800
 * and halves the stop cycle again (four samples per beat). Use the value
 * matching the channel bandwidth.                         [static] [measured] */
#define MT7996_SPECTRUM_W_BW		10u
#define   MT7996_SPECTRUM_BW_20			0u
#define   MT7996_SPECTRUM_BW_40			1u
#define   MT7996_SPECTRUM_BW_80			2u
#define   MT7996_SPECTRUM_BW_80P80		3u
#define   MT7996_SPECTRUM_BW_160		4u
#define   MT7996_SPECTRUM_BW_320		5u
/* Not consulted by the firmware capture path; the packer is fixed per
 * instance (ICAP0/1 none, ICAP2 64->128).                            [static] */
#define MT7996_SPECTRUM_W_BIT_WIDTH	11u
#define MT7996_SPECTRUM_W_ARCH		12u
/* PHY index = DFE bank of the capture mux (dfe.h) and ICAPCR8 band select.
 * 0, 1, 2 = band 0 (2.4 GHz), 1 (5 GHz), 2 (6 GHz).        [measured] [static] */
#define MT7996_SPECTRUM_W_PHY_IDX	13u
#define MT7996_SPECTRUM_W_EMI_START	14u
#define MT7996_SPECTRUM_W_EMI_END	15u
#define MT7996_SPECTRUM_W_EMI_MSB	16u
/* ICAPCR0.SRC_SEL (icap.h). 0 = DFE path, the only source that lands. */
#define MT7996_SPECTRUM_W_SRC		17u
#define MT7996_SPECTRUM_W_PATH		18u
#define MT7996_SPECTRUM_W_RX_GAIN	19u
/* Words 20, 21: reserved, 0. */

/* ------------------------------------------------------------------------ */
/* Tag 1 status reply: event TLV tag 0, 68 data bytes.             [measured] */
/* ------------------------------------------------------------------------ */
#define MT7996_SPECTRUM_EVT_STATUS_TAG	0u
#define MT7996_SPECTRUM_EVT_STATUS_LEN	68u	/* data bytes after the TLV header */
#define MT7996_SPECTRUM_EVT_FUNC	0u	/* data word 0: function */
#define   MT7996_SPECTRUM_FUNC_STATUS		1u
#define   MT7996_SPECTRUM_FUNC_DUMP		2u
#define MT7996_SPECTRUM_EVT_CAP_DONE	1u	/* data word 1: capture done */

/* ------------------------------------------------------------------------ */
/* Tag 2 dump request: 12 u32 words; replies are event TLV tag 1 with 1072  */
/* data bytes: six header words, then up to 256 sample words at data +48.   */
/* The firmware reads the window with the MCU and returns <= 256 words per  */
/* event; an event with count 0 ends the dump. On the stock TESTMODE WM the */
/* dump returns one empty event: read the window directly (iq_format.h).    */
/*                                                                 [measured] */
/* ------------------------------------------------------------------------ */
#define MT7996_SPECTRUM_DUMP_WORDS	12u
#define MT7996_SPECTRUM_DUMP_W_ADDR	0u
#define MT7996_SPECTRUM_DUMP_W_ADDR_OFS	1u
#define MT7996_SPECTRUM_DUMP_W_BANK	2u
#define MT7996_SPECTRUM_DUMP_W_BANK_SIZE 3u
#define MT7996_SPECTRUM_DUMP_W_PATH	4u
#define MT7996_SPECTRUM_DUMP_W_IQ_TYPE	5u
#define MT7996_SPECTRUM_EVT_DUMP_TAG	1u
#define MT7996_SPECTRUM_EVT_DUMP_LEN	1072u
#define MT7996_SPECTRUM_EVT_DUMP_W_PACKET 1u	/* data word 1: packet sequence */
#define MT7996_SPECTRUM_EVT_DUMP_W_BANK	2u
#define MT7996_SPECTRUM_EVT_DUMP_W_COUNT 3u	/* sample words in this event, <= 256 */
#define MT7996_SPECTRUM_EVT_DUMP_W_PATHS 4u
#define MT7996_SPECTRUM_EVT_DUMP_W_SAMPLES 5u
#define MT7996_SPECTRUM_EVT_DUMP_DATA	48u	/* byte offset of the sample words */
#define MT7996_SPECTRUM_EVT_DUMP_MAX	256u

/*
 * Prime: arm then retire with the same parameters                 [measured]
 *   ENABLE 1/0, NODE 0x00204000, LENGTH 2048, SRC 0, BW per channel,
 *   BAND_IDX = PHY_IDX = band; all other words 0.
 *
 * What the arm does, in order [static] (capture_setup.h has the details):
 * per ICAP instance 0..2: the capture clear and SW_RST_B pulse, the packer
 * (ICAP2), route(inst, type 1, enable), EN + SRC_SEL + RING, ICAPCR8 band
 * select, ADDR0 = [0, 0xffc], ICAPCR5 stop cycle; then CAPTURE on the three
 * instances; BN_CLK0 / 1 CAPTURE (phy.h); the capture configure (DFE CTRL,
 * MODE, LANE, CAPTRIG_CFG); the node's DFE mux fields on bank PHY_IDX; the
 * capture trigger (phy.h MT7996_PHY_CAPTRIG). Around arm and retire the
 * vendor test-mode manual-gain recipe saves and restores its PHY words.
 * The retire releases the route (cap_route.h +0x04) and clears ICAP
 * CAPTURE (all three instances when PHY index = 1). The DFE mux, lane mux, ICAP0
 * setup and the other route words persist, so after one prime the host or
 * an MCU program can capture by holding the route and triggering ICAP
 * directly (icap.h). An arm with no route written (no prime, no hold) counts
 * strobes but never completes.                                     [measured]
 *
 * Firmware differences:
 *  - The stock TESTMODE WM returns from the arm with the route still set and
 *    holds it until a status or retire command; its capture lands in the
 *    window (1024 beats = 51.2 us at 40 Msps).                    [measured]
 *  - Other TESTMODE WM builds release the route 50 us after the start, inside
 *    the arm command, and leave the shared route words on ICAP2's values, so
 *    ICAP0 lands nothing; hold the ICAP0 route and re-trigger.
 *                                                         [static] [measured]
 *  - The firmware's ICAP/SPECTRUM operating modes (op 2/4) are consumed only
 *    at WM init; switching them live has no effect.                 [static]
 *
 * Measured settings:
 *   band 0, 20 MHz channel: BW 0 -> 2 streams x 40 Msps
 *   band 0, 40 MHz channel: BW 1 -> 2 streams x 80 Msps
 *   band 1, 160 MHz channel: BW 4 -> 2 streams x 320 Msps
 *   band 2, 6 GHz 320 MHz channel: BW 5 -> 1 stream x 640 Msps
 * HAZARD: on a 20 MHz channel BW 1/2/4/5 only oversample: exactly 50/75/
 *   87.5/87.5 % of adjacent samples repeat and a rate fit gives nonsense.
 *   The channel bandwidth sets the data rate; no capture word sets an ADC
 *   clock (iq_format.h).
 */

#endif /* HW_MT7996_SPECTRUM_CMD_H */
