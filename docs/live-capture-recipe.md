<!-- SPDX-License-Identifier: BSD-3-Clause -->
# Live capture recipe (MT7981) — the reproducible path to a running capture

This is the exact, reproduced-on-hardware sequence that makes the MT7981's
ICAP/RBIST capture engine **arm and run against live RX** on the OpenWrt One.
It is the most important bring-up artifact in the repo: everything else
(the producer port, streaming, the web UI) builds on it.

This document is the capture half — arming the engine against live RX. The
firmware producer in [../firmware/streaming/](../firmware/streaming/README.md)
is the streaming half that drains the pool into the VITA 49 stream.

## The one thing that unblocked it: radios must be UP

Every earlier attempt saw "a frozen constant" in every buffer. The cause was
mundane: the RX front end had no power or tune, so nothing fed the DFE. The
fix is to enable the OpenWrt radios so there is live RX:

```sh
uci set wireless.radio0.disabled=0      # 2.4 GHz
uci set wireless.radio1.disabled=0      # 5 GHz
uci commit wireless
wifi up
```

After this, `iw dev` shows AP/monitor vifs tuned to real channels, and the
capture engine responds to live RX for the first time.

## Topology notes

- The MT7981 presents **two phys**: the lower index is 2.4 GHz (**band 0**),
  the higher is 5 GHz (**band 1**). Pick the `.../mt76` debugfs of the phy
  whose band you want to capture. The ICAP/DFE/route registers are one shared
  hardware block reachable from either phy's register proxy.
- The phy **index renumbers on every driver reload**. Resolve it each time:
  `ls /sys/class/ieee80211/`.

## The driver: flex `mt7915e.ko`

Built from the OpenWrt `mt76` tree with two local additions
(`md5 add88c8b…`, see [../firmware/README.md](../firmware/README.md) and the
build tree under `build/`):

- `icap_boot_mode` module parameter (`mcu.c`): sets `MT_SWDEF_MODE` **before**
  firmware download. `=0` normal (used here), `=1` ICAP boot.
- `icap_prime` debugfs knob (`testmode.c`): a flexible MCU-command sender. A
  `u64` written to it is an opcode in bits `[63:56]`:
  | op | action |
  |----|--------|
  | `0x00` | clear the 128-byte command buffer, set len=88 |
  | `0x01` | set one byte: offset in `[31:16]`, value in `[7:0]` |
  | `0x02` | set command length = `val & 0xffff` |
  | `0x13` | **fire** buffer as `MCU_CE_CMD(1)` (= cmd word `0x40001`) |
  | `0xff` | **fire** buffer as `MCU_EXT_CMD(RF_TEST=4)` |
  | `0x14` | **fire** buffer as the MT7981 native ICAP cmd `0x56ed` |

Load it in **normal** mode:

```sh
rmmod mt7915e
insmod /tmp/mt7915e_flex.ko icap_boot_mode=0
```

## The recipe (host-driven)

Shell helpers against one phy's debugfs `D`:

```sh
D=/sys/kernel/debug/ieee80211/phy8/mt76      # the 2.4 GHz phy
kb(){ echo "$1" > $D/icap_prime; }
sb(){ kb "$(printf '0x01000000%04x00%02x' "$1" "$2")"; }     # set byte off,val
setw(){ o=$1; v=$2; sb $o $((v&255)); sb $((o+1)) $(((v>>8)&255)); \
        sb $((o+2)) $(((v>>16)&255)); sb $((o+3)) $(((v>>24)&255)); }
# ce(action,function,value) -> MCU_CE_CMD(1), body <B3xII>
ce(){ kb 0x0; kb 0x020000000000000c; sb 0 "$1"; setw 4 "$2"; setw 8 "$3"; kb 0x1300000000000000; }
# EXT 0x56 capture-arm, body word4=enable, word16=len, word20/24=256
ext56(){ kb 0x0; sb 4 1; sb 17 0x40; sb 21 0x01; sb 25 0x01; kb 0x1400000000000000; }
rr(){ echo "$1" > $D/regidx; cat $D/regval; }
```

Sequence (BirriaSDR `Receiver.configure()` ported to the flex knob, with the
MT7981 capture command substituted):

```sh
# 1. RF_AT STARTRX config  (all return ret=0 in dmesg)
ce 0 2 0 ; sleep 1
ce 1 104 0          # RX setup
ce 1 106 65536      # RX_PATH = 1<<16
ce 1 18 2412000     # CHNL_FREQ in kHz = (2407 + 5*ch) * 1000, here ch1
ce 1 15 0
ce 1 71 0 ; ce 1 72 0 ; ce 1 73 0   # BW etc.
ce 1 1 13           # CMD
ce 1 1 2            # STARTRX
sleep 1
# 2. Arm the capture (MT7981 native command)
ext56 ; sleep 1
# 3. Observe
echo "CR0=$(rr 0x80023090) CR9=$(rr 0x800230b4) CR3=$(rr 0x8002309c) CR8=$(rr 0x800230b0)"
```

Expected: `CR0=0x401` (done), `CR9=0x3ffc`, `CR3=0x3ffc`, `CR8=0` (band 0).

## MT7981 vs MT7961 (BirriaSDR): the capture command differs

BirriaSDR (MT7961) arms the capture with **RF_TEST function 11**
(`ext(11,[1,0,0,NODE,256,256])`). On MT7981 that command returns `ret=0` but
does **not** arm the engine (`CR0=0x400`, `CR9=0`). The MT7981 capture command
is **EXT `0x56`**. Everything upstream — stock sniffer/monitor RX and the
`ce()` RF_AT STARTRX loop — is identical; neither chip needs a custom RF
bring-up, contrary to an earlier belief.

## What the buffers show (honest)

After a run, only **two words vary** between back-to-back captures:
`0xe0200000[3]` and `[7]`. Their Q halves are fixed (`0xffc1`, `0xc000`), so
they are telemetry/AGC, not IQ. `0xe0200000` is a capture **descriptor page**
(header `0x25800103 0x80026400 0x80002408 … 0x0000403c 0x40000000`). The bulk
16 KB region is static between captures — the sample **writer is backpressured
because nothing drains the capture FIFO/DMA ring** (`0x54000300`). Reading real
IQ needs the producer.

## Register map (MT7981, reached via `regidx`/`regval`)

| name | addr | note |
|------|------|------|
| ICAP CR0 | `0x80023090` | `0x401` done, `0x403` running, `0x24f3` ring start |
| ICAP CR8 (band) | `0x800230b0` | 0 = band0 (2.4), 1 = band1 (5) |
| ICAP CR9 (write ptr) | `0x800230b4` | advances to end on completion |
| ICAP CR2 (ADDR0_START) | `0x80023098` | |
| ICAP CR3 (ADDR0_END) | `0x8002309c` | `0x3ffc` for a 16 KB arm |
| DFE | `0x830ad000` / `+0x008` / `+0x440`/`+0x448`/`+0x460` | mode/mux/ctrl |
| route grant | `0x88009004` | `0x0f000000` on MT7981 |
| capture SRAM / descriptor | `0xe0200000` | descriptor + telemetry here |
| DMA ring | `0x54000300` | drained by the producer |
| A-die SPI master | `0x18004000` | chip id `0x79768b00` via soc.c SPI seq |
| SWDEF mode | `0x41143c` | 0 normal, 1 ICAP, 2 spectrum |

## Credits

The recipe is a direct port of **BirriaSDR** (Luigi Cruz, MIT —
<https://github.com/luigifcruz/BirriaSDR>): `Receiver.configure()`,
`arm_dense()`, and the `ce()`/`ext()` framing. The MT7981 register decode and
the EXT `0x56` capture command follow **W1700K-SDR** (h0m3us3r) and this project's own RE
of the stock MT7981 WM. See [../LICENSE](../LICENSE).
