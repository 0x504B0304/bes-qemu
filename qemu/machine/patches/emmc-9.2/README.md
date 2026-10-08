# QEMU SD/MMC backport

This package backports the common SD/MMC card implementation from official
[QEMU v9.2.0](https://gitlab.com/qemu-project/qemu/-/tree/v9.2.0/hw/sd)
to the project's pinned **QEMU v8.2.10**. It provides the upstream `emmc`
device on `SDBus`; board code creates the card and supplies its block backend.
It does not replace the host controller or implement a second MMC protocol.

`manifest.json` pins every original and resulting file by SHA256, and also
pins the patch itself. `tools/integrate_qemu.py` applies it offline using exact
line matches. All files are checked before any integration writes occur.
Repeating integration is a no-op for already patched files. Unknown versions,
local edits, invalid patches, and unexpected output fail explicitly. Mixed
original/patched files are accepted to recover an interrupted installation.
The patch intentionally preserves the original source files' license notices.

## Changes relative to upstream v9.2.0

- Keep QEMU 8.2's public command enum and its numeric values for OMAP; assign
  the internal SPI command kind a separate negative value.
- Use QEMU 8.2's `DeviceClass.reset` and VMState subsection pointer qualifiers.
- Store the SD CSD's 24-bit size using explicit bytes; QEMU 8.2 has no
  `st24_be_p` helper.
- Preserve PXA2XX's Meson source entry and trace events.
- Stop compiling the old `sdmmc-internal.c` command-name table, since the
  names now live in the common card implementation. Leave its file intact.

The other changes are upstream's SD/MMC implementation, common bus casts,
EXT_CSD constants, class declarations, and SD trace signatures. Existing
`sd-card`, `sd-card-spi`, and legacy callers remain supported. This package
does not modify OMAP/PXA2XX controllers.

The package also includes a local SDHCI correctness fix: the ADMA descriptor
length field is interpreted only for transfer actions. A separate valid
NOP/END descriptor must terminate a completed transfer without treating its
zero length as another 64 KiB transfer. Transfer length mismatches and incomplete
block counts still report errors. Split reads/writes and malformed transfers
are covered by the eMMC Qtest suite.

SDMA address writes now resume only an active SDMA transfer. Configuring an
idle host with a leftover block count must not move data or signal completion.
At a buffer boundary, SYSAD remains writable while DAT is inhibited, so a
driver can acknowledge the DMA interrupt and then resume at the new address.
Qtests cover idle setup, boundary resumption, and isolation from PIO reads.

The host also exposes an optional bus-master attribute callback for SoC
integration. It covers SDMA, ADMA data and all ADMA descriptor reads. The
BEST1700 wrapper uses its AON security register to select Secure/Nonsecure
transactions while retaining MPC/TGU checks. Machines without the callback
retain the common host's original attributes.

## Scope and limitations

The upstream card advertises eMMC 4.3 and 26/52 MHz operation. It supports
native MMC initialization, EXT_CSD/SWITCH, block I/O, and boot partition
selection. It does not provide complete modern eMMC functionality such as
RPMB, bus testing, or HS200 tuning. The upstream realization code requires
the complete block backend size to be a power of two. With zero boot partition
size, the user area begins at backing offset zero. Otherwise the layout is
boot0, boot1, then user area; disk construction must respect that offset.

## Provenance and licensing

Original release files are available under
`https://raw.githubusercontent.com/qemu/qemu/v9.2.0/` and
`https://raw.githubusercontent.com/qemu/qemu/v8.2.10/`, using the paths
listed in the manifest. `sd.c` and `include/hw/sd/sd.h` carry BSD-2-Clause
notices; `core.c` and `sdmmc-internal.h` carry GPL-2.0-or-later notices.
The Meson/trace changes accompany those upstream sources under QEMU's
licensing terms; see [QEMU COPYING](https://gitlab.com/qemu-project/qemu/-/blob/v9.2.0/COPYING).
The local compatibility edits are distributed under the corresponding
upstream file licenses.
The SDHCI fixes apply to the GPL-2.0-or-later host controller from v8.2.10;
they are local fixes, not part of the v9.2.0 card backport.
