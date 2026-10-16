# fwguard — crash self-heal for live firmware-patch testing

Boot-time guard on the OpenWrt One that makes custom WiFi-firmware
(`mt7981_rom_patch.bin`) testing survivable: a wedging patch auto-reverts to a
pristine "gold" copy on the next power-cycle, **no serial rescue needed**.

## Why
Loading a bad rom_patch can wedge the WiFi MCU hard (and a live
`rmmod/modprobe mt7915e` can take the whole device down — do NOT do that; test
via `reboot` only). fwguard runs at boot (~13s uptime) **before** the WiFi
driver loads the firmware (~13.9s), so it controls which patch the MCU gets.

## Install (one-time)
```sh
# stage pristine gold copies on the device (persist across reboot, unlike /tmp)
cp /lib/firmware/mediatek/mt7981_rom_patch.bin /root/rom_patch.gold
# install + enable the init script (START=01)
cp fwguard.init /etc/init.d/fwguard; chmod +x /etc/init.d/fwguard
/etc/init.d/fwguard enable
```

## Normal operation
Every boot with no arm flag: `cp /root/rom_patch.gold -> the firmware file`.
So every plain `reboot` returns the device to pristine firmware = always clean.

## Safe test cycle (validated 2026-10-13)
```sh
cp /my/custom_rom_patch.bin /root/rom_patch.test   # the patch to try
touch /root/fwguard.arm                            # one-shot arm
reboot                                             # NOT rmmod/modprobe
```
On that boot fwguard **consumes the arm flag first**, then installs the test
patch, so the MCU loads it once. Read results over SSH/regidx.
- If it misbehaves/wedges: **power-cycle**. The flag is already consumed, so the
  next boot restores gold → device comes back clean automatically.
- Validated: armed boot installs the test (md5 matched), unarmed boot reverts to
  gold (4740d7e7), WiFi healthy both times.

## Files on device
- `/etc/init.d/fwguard` (+ `/etc/rc.d/S01fwguard`) — the guard.
- `/root/rom_patch.gold` — pristine (md5 4740d7e74712830f9bbe034738930f17).
- `/root/rom_patch.test` — the candidate patch (optional).
- `/root/fwguard.arm` — one-shot arm flag (touch to arm; auto-removed at boot).
- `/root/fwguard.lastrun` — uptime(s) fwguard last ran (timing check).
