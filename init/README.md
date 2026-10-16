# init

Install and bring-up tooling for the board and the host.

These assume the OpenWrt image from [../docs/reproduce.md](../docs/reproduce.md)
is already flashed and running on the OpenWrt One, reached over SSH.

| File | Role |
| --- | --- |
| `deploy.sh` | rebuilds and stages the driver, firmware, scripts and responder, and enables the two boot services; requires a subsequent reboot. |
| `owrt-sdr.init` | late startup after boot-time preparation; starts the responder and drains capture on stop. |
| `owrt-sdr-prepare.init` | early boot firmware/module selection and failed-run fallback. |
| `host-link.sh` | brings up the host's 2.5 GbE interface for the direct link to the board's WAN port. |

## Guarded boot selection

Deployment stages files and enables two services; it does not reload the live
radio or reboot automatically. Verified `/root/wm.gold` and
`/root/mt7915e.stock.ko` backups must already exist.

`owrt-sdr-prepare` runs at S03, after the existing firmware/driver guards and
before S10 kmodloader. It writes and syncs
`/root/firmware/owrt-sdr.pending` before installing the staged firmware/module.
`owrt-sdr` runs at S99, confirms the selected files and custom debugfs interface,
brings up Wi-Fi, waits 25 seconds, then starts the control responder.

The persistent marker remains throughout operation. A successful service stop
first drains capture, stops the responder, then clears the marker. A crash,
power loss or startup/stop failure leaves it present. On the next boot S03
copies gold firmware and the stock module and declines to select SDR. After
inspection, `/etc/init.d/owrt-sdr-prepare rearm` permits another SDR boot.
It changes no running radio. An ordinary power-off without orderly shutdown
therefore intentionally requires rearming.

This recovery path was confirmed on a real cold boot: after an unclean power-off
the board came up on stock firmware with the marker still present, and an
explicit `rearm` was required before it would select the SDR pair again — the
guarded recovery behaving as designed.

To stay stock, leave the marker present and reboot once through preparation.
After stock restoration, disable both services:

```sh
/etc/init.d/owrt-sdr disable
/etc/init.d/owrt-sdr-prepare disable
```

Keep the original S01 firmware and S02 driver guards enabled. No `rmmod` or
`insmod` fallback is attempted: previous live reloads have wedged this board.
Offline mocks verify selection, fallback and stop ordering; actual boot order,
power-loss recovery and sustained capture remain untested with this revision.

## Network and access (the 2.5 GbE is a WAN port)

On a stock OpenWrt flash the ports are not interchangeable, and this trips up a
first deploy:

- **eth0, the 2.5 GbE port, is WAN.** It is a DHCP client, its zone REJECTs
  inbound by default, and SSH is not exposed on it. The SDR stream and control
  must use this port — it is the only one fast enough for 40 MSps.
- **eth1 is LAN** (`192.168.1.1`), default-open, with SSH. Manage the board
  here.

So the first bring-up is:

1. Flash the OpenWrt image from [../docs/reproduce.md](../docs/reproduce.md).
2. Reach the board over the **LAN port** (`192.168.1.1`); set a root key or
   password (LuCI or serial) if you have not.
3. Run `init/deploy.sh <lan-ip>`; the host's 2.5 GbE cables to the board's
   2.5 GbE (WAN) port for the stream and control.

`deploy.sh` and the boot service then set the SDR link up on the WAN port so you
do not hand-configure it:

- a fixed address on eth0 — `SDR_IP` in `owrt-sdr.init`, default
  `10.77.0.10/24`; the host uses `10.77.0.11/24`, with no gateway or DNS.
  This subnet avoids the laptop's Wi-Fi network. The development board now
  persists this address in `network.wan` as a static interface; a fresh board
  still needs that UCI configuration or the SDR service's address setup.
- an inbound ACCEPT for **udp/4991** (VITA 49.2 control) on the WAN zone,
  `Allow-VITA49-Control`. The IQ stream is outbound and needs no rule.

To use a different SDR-link address, edit `SDR_IP` in `owrt-sdr.init` and point
the host at the matching peer. SSH is intentionally left closed on the WAN/SDR
link; manage over LAN. Only open SSH on the SDR link if it is a direct, trusted
cable, and never expose it to an untrusted network.

## Use

```sh
init/host-link.sh enx00051b512a99            # host side of the 2.5 GbE link
init/deploy.sh 10.77.0.10 ~/.ssh/owrt_one # board: driver + firmware + scripts
```

After `deploy.sh`, reboot to select the staged pair. The late service waits
25 seconds before starting the responder. Then start a capture with
[../tools/live/README.md](../tools/live/README.md). The board's direct-link address and the reboot-revert behaviour are described
in the sections above.

## Licence

MIT, © 2026 CEMAXECUTER LLC. See [../LICENSE](../LICENSE).
