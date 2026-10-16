<!-- SPDX-License-Identifier: MIT -->
# release

What a tagged release ships, how it is built, and why no proprietary firmware is
redistributed. For the step-by-step bring-up see the project
[README "Getting started"](../README.md#getting-started).

## Assets

| Asset | What it is |
| --- | --- |
| `openwrt-one-sdr-<ver>-squashfs-sysupgrade.itb` | a flashable OpenWrt One image at the pinned OpenWrt version, carrying the **stock** MediaTek Wi-Fi firmware exactly as OpenWrt ships it, plus this project's mt76 SDR driver. Flash it like any OpenWrt sysupgrade. |
| `openwrt-one-sdr-<ver>-sdr-tools.tar.gz` | everything the host needs to bring the board up without a build tree: `setup.sh`, the pre-assembled firmware patcher (`apply-producer.py` + `wm_regions.py`), the pre-built SDR driver and a stock driver for recovery, the cross-compiled on-device tools, the capture/control scripts, and the boot guards and services. |
| `SHA256SUMS` | checksums for every asset above. |

## The firmware, and why it is never in a release

The MT7981 Wi-Fi firmware is MediaTek's proprietary blob. This project **does not
redistribute it, patched or unpatched** — not in the repository, not in a release.

What it ships instead is the *patch*: this project's own nds32 DMA producer
(pre-assembled, MIT) and the few byte edits that redirect the firmware's command
dispatcher to it. `apply-producer.py` applies that patch to the stock blob that
is **already on the user's own board** (and already in the OpenWrt image, which
OpenWrt itself distributes). The SDR firmware is therefore produced locally, on
the user's machine, from the user's own copy — and stays there.

`apply-producer.py` refuses any input whose MD5 is not the pinned stock blob, and
its output is byte-identical to building the firmware from source with
`firmware/streaming/build.py --pool`.

## Building the assets

From a completed source build (see [docs/reproduce.md](../docs/reproduce.md)) the
release assets are assembled by `release/pack.sh`, which copies the OpenWrt image,
cross-compiles the on-device tools, pre-assembles the producer into
`apply-producer.py`, lays out the `sdr-tools` bundle, and writes `SHA256SUMS`. The
image and the compiled binaries are build outputs and are not committed to the
repository (see `.gitignore`); the release is how they are distributed, with the
corresponding source being this repository.

## Licences

The image is OpenWrt (GPL and others) plus this project's ISC driver, carrying
MediaTek's own redistributable stock firmware package. The `sdr-tools` bundle is
this project's MIT host/firmware tooling and ISC driver. Full texts in
[../LICENSES/](../LICENSES/).
