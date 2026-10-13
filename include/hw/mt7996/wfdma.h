/* SPDX-License-Identifier: MIT */
/*
 * MT7996 host WFDMA (PCIe host DMA engine), host rings and descriptors.
 *
 * ADDRESS SPACE: MT7996 PCIe BAR0 offsets. Every register here is below
 * 0x100000 and is accessed directly, with no remap window.
 *
 *   BAR 0xd4000  WFDMA0, primary PCIe interface (all rings used here)
 *   BAR 0xd5000  WFDMA1 (unused by the ordinary MT7996 path)
 *   BAR 0xd7000  WFDMA EXT_CSR (host config, HIF_MISC)
 *   BAR 0xd8000  WFDMA0 bank of the second PCIe interface (+0x4000)
 *
 * The 0xd0000..0xdffff BAR range is the fixed-map image of CONN_INFRA bus
 * address 0x7c020000 (so WFDMA0 = bus 0x7c024000, also reachable as
 * 0x18024000 through the L1 window, see host.h).                   [static]
 * Host DMA0 reads healthy at BAR 0xd0000/0xd4000 even while the WiFi-die
 * windows return 0xdead2117 (host.h).                              [measured]
 *
 * Ring control: each hardware ring has four 32-bit registers at a 0x10 stride
 * (BASE, CNT, CIDX, DIDX); TX ring n at 0xd4300 + n*0x10, RX ring n at
 * 0xd4500 + n*0x10.
 */
#ifndef HW_MT7996_WFDMA_H
#define HW_MT7996_WFDMA_H

#include "../hw_bits.h"

#define MT7996_WFDMA0_BASE			0x000d4000u
#define MT7996_WFDMA0_SIZE			0x00001000u
#define MT7996_WFDMA1_BASE			0x000d5000u
#define MT7996_WFDMA_EXT_CSR_BASE		0x000d7000u
#define MT7996_WFDMA0_PCIE1_BASE		0x000d8000u	/* second PCIe interface */
#define MT7996_WFDMA0_BUS_ADDR			0x7c024000u	/* same block, CONN_INFRA view */

/* ------------------------------------------------------------------------ */
/* Reset, busy and interrupt-routing                                        */
/* ------------------------------------------------------------------------ */

/* WFDMA0 reset control. RW. Init toggles bits 5:4 low then high; reset
 * asserted while clear.                                             [static] */
#define MT7996_WFDMA0_RST			0x000d4100u
#define   WFDMA0_RST_LOGIC_RST			HW_BIT(4)
#define   WFDMA0_RST_DMASHDL_ALL_RST		HW_BIT(5)
/* other bits undecoded */

/* Busy-source enable for HIF_MISC.BUSY. RW. Set to 0x7 before polling idle.
 *                                                         [static, measured] */
#define MT7996_WFDMA0_BUSY_ENA			0x000d413cu
#define   WFDMA0_BUSY_ENA_TX_FIFO0		HW_BIT(0)
#define   WFDMA0_BUSY_ENA_TX_FIFO1		HW_BIT(1)
#define   WFDMA0_BUSY_ENA_RX_FIFO		HW_BIT(2)

/* Stock vendor host sets bit 22 here before download; name undecoded. [static] */
#define MT7996_WFDMA0_CFG_D4150			0x000d4150u
/* RX interrupt redirect to the second PCIe interface. RW. The stock vendor
 * host ORs 0xe0 (rings 5..7) before download; the reference dual-PCIe mode sets bit 3.
 *                                                                   [static] */
#define MT7996_WFDMA0_RX_INT_PCIE_SEL		0x000d4154u
#define   WFDMA0_RX_INT_SEL_RING(n)		HW_BIT(n)

/* ------------------------------------------------------------------------ */
/* Firmware <-> host recovery (SER) handshake                               */
/* ------------------------------------------------------------------------ */

/* Firmware status to host. Host reads, then writes the whole observed word
 * back to acknowledge (W1C use). Accepted words: any of ERROR_MASK or
 * WDT_MASK.                                                         [static] */
#define MT7996_WFDMA0_MCU_CMD			0x000d41f0u
#define   MCU_CMD_STOP_DMA			HW_BIT(2)
#define   MCU_CMD_RESET_DONE			HW_BIT(3)
#define   MCU_CMD_RECOVERY_DONE			HW_BIT(4)
#define   MCU_CMD_NORMAL_STATE			HW_BIT(5)
#define   MCU_CMD_ERROR_MASK			HW_GENMASK(5, 1)	/* bit 1 unnamed */
#define   MCU_CMD_WM_WDT			HW_BIT(30)
#define   MCU_CMD_WA_WDT			HW_BIT(31)
#define   MCU_CMD_WDT_MASK			HW_GENMASK(31, 30)

/* Firmware-to-host interrupt enable; watchdog bits 31:30 cleared during a
 * full reset and restored afterwards. RW.                           [static] */
#define MT7996_WFDMA0_MCU_HOST_INT_ENA		0x000d41f4u

/* Host notification to firmware: BAR 0x2108 (MCU-side WFDMA0 window, bus
 * 0x54000108). WO from the host's point of view.                    [static]
 * Partial recovery: host writes DMA_STOPPED, waits fw RESET_DONE; resets host
 * rings; writes DMA_INIT, waits fw RECOVERY_DONE; writes RESET_DONE, waits fw
 * NORMAL_STATE; re-enables DMA. Reference timeout 30 s per wait.     [static] */
#define MT7996_MCU_INT_EVENT			0x00002108u
#define   MCU_INT_EVENT_DMA_STOPPED		HW_BIT(0)
#define   MCU_INT_EVENT_DMA_INIT		HW_BIT(1)
#define   MCU_INT_EVENT_RESET_DONE		HW_BIT(3)

/* ------------------------------------------------------------------------ */
/* Host interrupt source / mask                                             */
/* ------------------------------------------------------------------------ */

/* Interrupt status. Host writes the observed bits back to acknowledge (W1C).
 *                                                                   [static] */
#define MT7996_WFDMA0_INT_SOURCE_CSR		0x000d4200u
/* Interrupt enable mask. RW. 0 = all host IRQ delivery masked (our loader
 * keeps it 0 and polls ring indices).                     [static, measured] */
#define MT7996_WFDMA0_INT_MASK_CSR		0x000d4204u
#define MT7996_WFDMA0_PCIE1_INT_SOURCE_CSR	0x000d8200u
#define MT7996_WFDMA0_PCIE1_INT_MASK_CSR	0x000d8204u
/* Bit assignment for the MT7996 single-PCIe configuration. The IRQ bit is NOT
 * the ring number in general.                                       [static] */
#define   WFDMA_INT_RX_DONE_WM			HW_BIT(0)	/* RX ring 0 */
#define   WFDMA_INT_RX_DONE_WA			HW_BIT(1)	/* RX ring 1 */
#define   WFDMA_INT_RX_DONE_WA_MAIN		HW_BIT(2)	/* RX ring 2, band 0/1 TX-free */
#define   WFDMA_INT_RX_DONE_WA_TRI		HW_BIT(3)	/* RX ring 3, band 2 TX-free */
#define   WFDMA_INT_RX_DONE_BAND0		HW_BIT(12)	/* RX ring 4 */
#define   WFDMA_INT_RX_DONE_BAND2		HW_BIT(13)	/* RX ring 5 */
#define   WFDMA_INT_TX_DONE_BAND2		HW_BIT(15)	/* dual-PCIe TX ring 21 */
#define   WFDMA_INT_TX_DONE_MCU_WA		HW_BIT(22)	/* TX ring 20 */
#define   WFDMA_INT_TX_DONE_FWDL		HW_BIT(26)	/* TX ring 16 */
#define   WFDMA_INT_TX_DONE_MCU_WM		HW_BIT(27)	/* TX ring 17 */
#define   WFDMA_INT_MCU_CMD			HW_BIT(29)	/* see MCU_CMD 0xd41f0 */
#define   WFDMA_INT_TX_DONE_BAND0		HW_BIT(30)	/* TX ring 18 */
#define   WFDMA_INT_TX_DONE_BAND1		HW_BIT(31)	/* TX ring 19 */

/* ------------------------------------------------------------------------ */
/* Global configuration                                                     */
/* ------------------------------------------------------------------------ */

/* WFDMA0 global config. RW.                               [static, measured]
 * Cold enable sets TX_DMA_EN | RX_DMA_EN | OMIT_RX_INFO_PFET2 | EXT_EN |
 * OMIT_TX_INFO (0x14200005); disable clears 0x18200005. */
#define MT7996_WFDMA0_GLO_CFG			0x000d4208u
#define   WFDMA0_GLO_CFG_TX_DMA_EN		HW_BIT(0)
#define   WFDMA0_GLO_CFG_RX_DMA_EN		HW_BIT(2)
#define   WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2	HW_BIT(21)
#define   WFDMA0_GLO_CFG_EXT_EN			HW_BIT(26)
#define   WFDMA0_GLO_CFG_OMIT_RX_INFO		HW_BIT(27)
#define   WFDMA0_GLO_CFG_OMIT_TX_INFO		HW_BIT(28)
#define   WFDMA0_GLO_CFG_START_BITS		0x14200005u
#define   WFDMA0_GLO_CFG_STOP_BITS		0x18200005u
/* bits 1/3 are TX/RX DMA busy in sibling WFDMA blocks [inferred]; rest undecoded */

/* Reset all TX/RX DMA pointers: write all-ones; self-clearing, do not read
 * back as a completion indication. WO.                              [static] */
#define MT7996_WFDMA0_RST_DTX_PTR		0x000d420cu

/* RX pause thresholds: two 12-bit fields (27:16 and 11:0). RW.
 * Reference values: 0x000c000c, 0x00010008, 0x00010008, 0x20.        [static] */
#define MT7996_WFDMA0_PAUSE_RX_Q_45_TH		0x000d4268u
#define MT7996_WFDMA0_PAUSE_RX_Q_67_TH		0x000d426cu
#define MT7996_WFDMA0_PAUSE_RX_Q_89_TH		0x000d4270u
#define MT7996_WFDMA0_PAUSE_RX_Q_RRO_TH		0x000d427cu
#define   WFDMA0_PAUSE_TH_HI			HW_GENMASK(27, 16)
#define   WFDMA0_PAUSE_TH_LO			HW_GENMASK(11, 0)

/* Global config extension 0. RW. Init ORs RX_WB_RXD | WED_MERGE_MODE
 * (0x00044000); stock vendor host ORs bit 14 before download.         [static] */
#define MT7996_WFDMA0_GLO_CFG_EXT0		0x000d42b0u
#define   WFDMA0_GLO_CFG_EXT0_WED_MERGE_MODE	HW_BIT(14)
#define   WFDMA0_GLO_CFG_EXT0_RX_WB_RXD		HW_BIT(18)
#define   WFDMA0_GLO_CFG_EXT0_OUTSTAND		HW_GENMASK(27, 24)

/* Global config extension 1. RW. CALC_MODE set before prefetch programming,
 * TX_FCTRL_MODE after the idle poll.                                [static] */
#define MT7996_WFDMA0_GLO_CFG_EXT1		0x000d42b4u
#define   WFDMA0_GLO_CFG_EXT1_TX_FCTRL_MODE	HW_BIT(28)
#define   WFDMA0_GLO_CFG_EXT1_CALC_MODE		HW_BIT(31)

/* Delayed-interrupt configuration, cleared to 0 at init. RW.          [static] */
#define MT7996_WFDMA0_PRI_DLY_INT_CFG(n)	(0x000d42f0u + (n) * 4u)	/* n = 0..2 */

/* ------------------------------------------------------------------------ */
/* Ring control registers                                                   */
/* ------------------------------------------------------------------------ */

#define MT7996_WFDMA0_TX_RING_BASE(n)		(0x000d4300u + (n) * 0x10u)
#define MT7996_WFDMA0_TX_RING_CNT(n)		(0x000d4304u + (n) * 0x10u)
#define MT7996_WFDMA0_TX_RING_CIDX(n)		(0x000d4308u + (n) * 0x10u)
#define MT7996_WFDMA0_TX_RING_DIDX(n)		(0x000d430cu + (n) * 0x10u)
#define MT7996_WFDMA0_RX_RING_BASE(n)		(0x000d4500u + (n) * 0x10u)
#define MT7996_WFDMA0_RX_RING_CNT(n)		(0x000d4504u + (n) * 0x10u)
#define MT7996_WFDMA0_RX_RING_CIDX(n)		(0x000d4508u + (n) * 0x10u)
#define MT7996_WFDMA0_RX_RING_DIDX(n)		(0x000d450cu + (n) * 0x10u)
/*
 * BASE: RW, 32-bit bus address of the descriptor array (descriptors must sit
 *       below 4 GiB; 4-byte alignment).
 * CNT:  RW, number of descriptors.
 * CIDX: RW, CPU index. TX: host publishes head (after a write barrier).
 *       RX: host publishes the last re-armed slot; one slot stays empty.
 * DIDX: RO in use, DMA index; host may write 0 only while the engine is
 *       stopped (ring bind).                                 [static, measured]
 * Bind order used: CIDX=0, DIDX=0, CNT, BASE, read back all four, then for RX
 * publish CIDX = posted count.                                     [measured]
 */
/* The same four registers as offsets inside one ring's block, from its BASE(n)
 * address.                                                [static, measured] */
#define MT7996_WFDMA_RING_BASE			0x0u
#define MT7996_WFDMA_RING_CNT			0x4u
#define MT7996_WFDMA_RING_CIDX			0x8u
#define MT7996_WFDMA_RING_DIDX			0xcu
#define MT7996_WFDMA_RING_REGS_BYTES		0x10u

/* Prefetch (EXT_CTRL) per ring: (base << 16) | depth; base advances by
 * depth << 4 in allocation order, reserving slots for unprogrammed rings.
 * Reference single-PCIe values: TX16 0x00000002, TX17 0x00200002,
 * TX20 0x00400002, RX0 0x01600002, RX1 0x01800002.          [static, measured] */
#define MT7996_WFDMA0_TX_RING_EXT_CTRL(n)	(0x000d4600u + (n) * 4u)
#define MT7996_WFDMA0_RX_RING_EXT_CTRL(n)	(0x000d4680u + (n) * 4u)
#define   WFDMA_RING_EXT_CTRL_BASE		HW_GENMASK(31, 16)
#define   WFDMA_RING_EXT_CTRL_DEPTH		HW_GENMASK(7, 0)
/* bits 15:8 not checked on readback (reserved) */

/* Hardware ring assignment, single PCIe.                  [static, measured] */
#define MT7996_TXQ_FWDL				16u	/* FW_SCATTER payload, 128 desc */
#define MT7996_TXQ_MCU_WM			17u	/* download-era commands, 256 desc */
#define MT7996_TXQ_BAND0			18u	/* data, 2048 desc (band 1 shares) */
#define MT7996_TXQ_BAND2			19u	/* data, 2048 desc */
#define MT7996_TXQ_MCU_WA			20u	/* runtime commands, 256 desc */
#define MT7996_RXQ_MCU_WM			0u	/* WM events, 512 desc reference */
#define MT7996_RXQ_MCU_WA			1u	/* WA events, 1024 desc */
#define MT7996_RXQ_TXFREE_BAND0			2u	/* 512 desc */
#define MT7996_RXQ_TXFREE_BAND2			3u	/* 512 desc */
#define MT7996_RXQ_BAND0			4u	/* data, 1536 desc */
#define MT7996_RXQ_BAND2			5u	/* data, 1536 desc */
/*
 * Ring usage:
 *  - The boot ROM services download commands on TX17 and answers on RX0.
 *  - Once firmware runs, every command (WM-, WA- or both-destined, chosen by
 *    s2d_index) goes on TX20 and is forwarded WA -> WM. Started firmware
 *    answers on RX1 whichever TX ring carried the command.        [measured]
 *  - The MCU sees the host ring registers at bus 0x7c024000 + offset (e.g.
 *    TX20 CIDX at 0x7c024448).                                     [measured]
 *  - RX0 also receives anything the MCU's own DMA0 TX0 sends; with 8 KiB
 *    buffers it carries 1280 MB/s sustained.                       [measured]
 *  - A command ring with one unconsumed descriptor blocks later commands;
 *    TX20 never drains if the WA was not started.                  [measured]
 *
 * HAZARD: an RX0 ring of 4096 descriptors accepts the CNT write and reads it
 *         back, but the ring never delivers (the firmware download then times
 *         out at the first semaphore reply). 1536 works.            [measured]
 * HAZARD: stopping a device-side producer before its MCU DMA TX0 has drained
 *         into RX0 strands a descriptor and wedges the WM until the WF
 *         subsystem is reset and firmware reloaded.                 [measured]
 * HAZARD: storage exposed through a ring may be written by the device until
 *         the WF subsystem reset (conn.h) has run and bus mastering is off; a
 *         stop/idle observation alone is not drain.                  [static]
 */

/* ------------------------------------------------------------------------ */
/* EXT_CSR                                                                  */
/* ------------------------------------------------------------------------ */

/* Host band routing between the two PCIe interfaces. RW. The stock vendor
 * host clears bits 22 and 21, sets bit 20 and bit 0 before download.  [static] */
#define MT7996_WFDMA_HOST_CONFIG		0x000d7030u
#define   WFDMA_HOST_CONFIG_PDMA_BAND		HW_BIT(0)
#define   WFDMA_HOST_CONFIG_BAND0_PCIE1		HW_BIT(20)
#define   WFDMA_HOST_CONFIG_BAND1_PCIE1		HW_BIT(21)
#define   WFDMA_HOST_CONFIG_BAND2_PCIE1		HW_BIT(22)

/* HIF misc status. RO. BUSY reflects the sources enabled in BUSY_ENA; poll it
 * clear (reference: 101 reads, 10 us apart) before enabling. Clear does NOT
 * prove outstanding PCIe writes have drained.             [static, measured] */
#define MT7996_WFDMA_HIF_MISC			0x000d7044u
#define   WFDMA_HIF_MISC_BUSY			HW_BIT(0)

/* AXI outstanding limits, programmed per link speed/width for dual PCIe. */
#define MT7996_WFDMA_AXI_R2A_CTRL		0x000d7500u
#define   WFDMA_AXI_R2A_CTRL_OUTSTAND		HW_GENMASK(4, 0)
#define MT7996_WFDMA_AXI_R2A_CTRL2		0x000d7508u
#define   WFDMA_AXI_R2A_CTRL2_OUTSTAND		HW_GENMASK(31, 28)

/* PCIe interface recognition word (dual-PCIe pairing).               [static] */
#define MT7996_PCIE_RECOG_ID			0x000d7090u
#define   PCIE_RECOG_ID_ID			HW_GENMASK(30, 0)
#define   PCIE_RECOG_ID_SEM			HW_BIT(31)

/* ------------------------------------------------------------------------ */
/* Descriptor (16 bytes, four little-endian words, 4-byte aligned)          */
/* ------------------------------------------------------------------------ */

#define MT7996_WFDMA_DESC_BYTES			16u
#define MT7996_WFDMA_DESC_BUF0			0u	/* buffer 0 address [31:0] */
#define MT7996_WFDMA_DESC_CTRL			4u
#define MT7996_WFDMA_DESC_BUF1			8u
#define MT7996_WFDMA_DESC_INFO			12u
/* ctrl word.                                              [static, measured] */
#define   WFDMA_DESC_CTRL_SD_LEN1		HW_GENMASK(13, 0)	/* TX buffer 1 length */
#define   WFDMA_DESC_CTRL_LAST_SEC1		HW_BIT(14)	/* TX only */
#define   WFDMA_DESC_CTRL_DROP			HW_BIT(14)	/* RX only: drop */
#define   WFDMA_DESC_CTRL_M_DONE		HW_BIT(15)
#define   WFDMA_DESC_CTRL_SD_LEN0		HW_GENMASK(29, 16)	/* length / RX capacity */
#define   WFDMA_DESC_CTRL_LAST_SEC0		HW_BIT(30)	/* last segment */
#define   WFDMA_DESC_CTRL_DMA_DONE		HW_BIT(31)	/* set by HW on completion */
/* RX-only interpretations of ctrl.                                  [static] */
#define   WFDMA_DESC_CTRL_VER			HW_BIT(7)	/* versioned drop reason valid */
#define   WFDMA_DESC_CTRL_TO_HOST_A		HW_BIT(12)	/* drop */
#define   WFDMA_DESC_CTRL_PN_CHK_FAIL		HW_BIT(13)
/* RX buf1: buffer address [35:32] in 3:0; when CTRL_VER, drop reason in 15:12
 * (1 = drop, 2 = drop unless info.DMA_FRAG, other = drop on PN_CHK_FAIL). */
#define   WFDMA_DESC_RX_BUF1_ADDR_HI		HW_GENMASK(3, 0)
#define   WFDMA_DESC_RX_BUF1_REASON		HW_GENMASK(15, 12)
#define     WFDMA_DESC_RX_BUF1_REASON_NORMAL	0u
#define     WFDMA_DESC_RX_BUF1_REASON_REPEAT	1u	/* drop */
#define     WFDMA_DESC_RX_BUF1_REASON_OLDPKT	2u	/* drop unless info.DMA_FRAG */
/* TX info: buffer 0 address [35:32] in 3:0, buffer 1 address [35:32] in 19:16. */
#define   WFDMA_DESC_TX_INFO_SDP0_HI		HW_GENMASK(3, 0)
#define   WFDMA_DESC_TX_INFO_SDP1_HI		HW_GENMASK(19, 16)
#define   WFDMA_DESC_INFO_DMA_FRAG		HW_BIT(9)
/*
 * Data buffers are 36-bit bus addresses; descriptor arrays 32-bit.
 * TX command packets: one descriptor, 1..4096 bytes, LAST_SEC0 set; keep two
 * slots unused. RX: post with DMA_DONE clear and the capacity in SD_LEN0;
 * 14-bit length field, so 8 KiB is the largest power-of-two buffer.
 * Initialise every descriptor with DMA_DONE set.                   [measured]
 * Host-side PCIe DMA is not cache-coherent on the W1700K SoC: sync/invalidate
 * each RX buffer before reading it.                                [measured]
 */
#define MT7996_WFDMA_RX_BUF_MAX			0x3fffu
#define MT7996_WFDMA_TX_CMD_MAX			4096u

#endif /* HW_MT7996_WFDMA_H */
