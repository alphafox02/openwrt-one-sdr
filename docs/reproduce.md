# Reproducing the build

This document pins the external inputs
and gives the steps in dependency order. There is no single top-level `make`
yet; each component builds with its own script, listed below. The on-board
bring-up and the guarded-boot recovery have been validated on hardware
(including a real cold boot); a full from-scratch SDK/image rebuild has not yet
been re-run end to end from a clean checkout.

## Pinned inputs

| Input | Pin | Where it comes from |
| --- | --- | --- |
| OpenWrt (kernel, mt76 driver, image) | tag `v24.10.0-rc2`, commit `66e76aa94f0508d63a75fac2739a5bc0edb021ec` | `github.com/openwrt/openwrt.git`, target `mediatek/filogic`, device OpenWrt One |
| mt76 package (inside OpenWrt) | version `2024.10.28~c246fa54` | fetched by the OpenWrt build |
| nds32 cross toolchain | GNU Binutils `2.42` + GCC `10.5.0` | built from the GNU release tarballs (GCC 11 dropped nds32) |
| MediaTek Wi-Fi firmware | the copy the OpenWrt image already carries | never fetched or committed here |

The MediaTek firmware is proprietary and is not part of this repository. The
firmware build reads a copy from `vendor/` (kept out of git by `.gitignore`) and
asserts its MD5, so the patch applies to a known image.

## 1. OpenWrt base and the mt76 driver

Build OpenWrt for the OpenWrt One at the pinned commit. This produces the
flashable image, the cross toolchain in `staging_dir`, and the mt76 source and
build tree under `build_dir`.

```sh
git clone https://github.com/openwrt/openwrt.git build/openwrt
git -C build/openwrt checkout 66e76aa94f0508d63a75fac2739a5bc0edb021ec
cd build/openwrt
./scripts/feeds update -a && ./scripts/feeds install -a
# select target "MediaTek Ralink ARM" / subtarget "Filogic" / device "OpenWrt One"
make menuconfig
make -j"$(nproc)"
```

## 2. The driver patches

The capture knobs and the VITA 49 UDP sender are patches on OpenWrt's mt76.
Apply them to the mt76 source in the build tree and rebuild only the mt7915
objects (a full SDK rebuild is unnecessary and memory-heavy):

```sh
# in build/openwrt/build_dir/.../mt76-2024.10.28~c246fa54:
# From pristine mt76 root (not its mt7915 subdirectory):
#   patch -p1 < /path/to/kernel/mt7915-icap/complete-local.patch
python3 kernel/mt7915-icap/rebuild.py          # rebuilds mt7915e.ko
```

The driver patch changes `struct mt7915_dev`, so all mt7915 objects rebuild
together; `rebuild.py` with no arguments does that. See
[../kernel/mt7915-icap/README.md](../kernel/mt7915-icap/README.md).

## 3. The nds32 toolchain

The firmware producer is nds32 and needs GCC 10.5 (the last with nds32) plus
Binutils 2.42, built as a freestanding cross toolchain:

```sh
# binutils 2.42, then gcc 10.5.0, configured --target=nds32le-elf
#   --disable-libssp --without-headers --enable-languages=c --disable-nls
# install to a prefix on PATH; copy nds32le-elf/lib/ldscripts to tools/ldscripts
```

## 4. The firmware producer

Patch a local copy of the stock firmware with the DMA producer and the
dedicated-pool partition:

```sh
python3 firmware/streaming/build.py /tmp/fw --pool   # writes wm-window-dma.bin
```

See [../firmware/streaming/README.md](../firmware/streaming/README.md).

## 5. The host tools

```sh
make -C host/spectrum                                    # viewer (zlib + FFTW3f)
cc -O2 -I host/stream/include -o vita49-shm host/stream/vita49-shm.c -lrt
cc -O2 -o iq-vita49 host/stream/iq-vita49.c
# cross-compile the on-device control daemon with the OpenWrt toolchain:
build/openwrt/staging_dir/toolchain-*/bin/aarch64-openwrt-linux-musl-gcc \
    -O2 -o iq-c2d tools/live/iq-c2d.c
```

## 6. Install on the board

Flash the OpenWrt image from step 1 (`sysupgrade`), then deploy the rebuilt
driver, the patched firmware, and the control scripts. The exact device setup —
the direct 2.5 GbE link, bringing the radio up, and the recovery procedure after
a reboot (selection depends on the guarded SDR services) — is in
[../init/README.md](../init/README.md).

## Distributing a built image

A flashable OpenWrt image may be built and distributed under its own licences,
provided the corresponding source is offered alongside it (GPL-2.0 §3); all of
this project's source is here, and the OpenWrt/Linux base is pinned above. The
MediaTek firmware blob in such an image is the one OpenWrt itself ships, under
MediaTek's redistribution terms. The modified firmware is produced on the board
from that blob by step 4 and is not distributed. A pre-patched MediaTek firmware
binary must not be redistributed.

## Status

Pristine mt76 plus the consolidated driver patch compiled all 11 driver objects
and linked in the existing SDK. The dedicated-pool firmware rebuild produced MD5
`82db57e0e100a6467bb324b67c8e4662`. A completely fresh SDK/image build,
byte-identical cross-machine checks and live validation remain open work.
