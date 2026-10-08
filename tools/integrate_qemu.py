#!/usr/bin/env python3
"""Install owned sources and one Meson entry without accumulating patch stages."""
from pathlib import Path
import argparse
import json
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.source_layout import ROOT, sources
from tools.runtime_schema import decoder_source
from tools.package_config import header, load_package
from tools.qemu_emmc_backport import prepare_emmc_backport
from tools.qemu_backport import prepare_backport
from tools.qemu_backport import prepare_backport_stack


def require_mpc(kconfig):
    """The ARM_V7M source bundle subclasses tz-mpc even in minimal builds."""
    matches = list(re.finditer(r"(?m)^config ARM_V7M\n", kconfig))
    if len(matches) != 1:
        raise ValueError('unrecognized QEMU ARM_V7M Kconfig layout')
    start = matches[0].end()
    following = re.search(r"(?m)^(?:config|menuconfig) ", kconfig[start:])
    end = start + following.start() if following else len(kconfig)
    block = kconfig[start:end]
    if re.search(r"(?m)^\s+select TZ_MPC(?:\s*#.*)?$", block):
        return kconfig
    boolean = re.search(r"(?m)^    bool\n", block)
    if not boolean:
        raise ValueError('unrecognized QEMU ARM_V7M Kconfig layout')
    insert = start + boolean.end()
    return kconfig[:insert] + '    select TZ_MPC\n' + kconfig[insert:]


def install(qemu, root=ROOT):
    selected = sources(root)
    package_header = header(load_package(Path(root)/'qemu/soc/best1503/config.json'))
    directory = Path(qemu) / 'hw/arm'
    meson = directory / 'meson.build'
    text = meson.read_text()
    # Accept the prior handoff's single ARM_V7M entry or a pristine QEMU tree.
    # Do not rewrite other boards or accept an ambiguous integration state.
    pattern = r"arm_ss\.add\(when: 'CONFIG_ARM_V7M',\s*if_true: files\('miband\.c'.*?\)\)"
    matches = list(re.finditer(pattern, text, flags=re.S))
    if len(matches) > 1:
        raise ValueError('multiple emulator Meson entries')
    cpp = [p.name for p in selected.values() if p.suffix == '.c']
    entry = "arm_ss.add(when: 'CONFIG_ARM_V7M', if_true: files(" + ", ".join(repr(p) for p in cpp) + '))'
    if matches:
        span = matches[0].span()
        updated = text[:span[0]] + entry + text[span[1]:]
    else:
        anchor = "arm_ss.add(when: 'CONFIG_ARM_V7M', if_true: files('armv7m.c'))"
        if text.count(anchor) != 1 or "'miband.c'" in text:
            raise ValueError('unrecognized QEMU Meson layout')
        updated = text.replace(anchor, anchor+'\n'+entry)
    kconfig = directory / 'Kconfig'
    kconfig_text = kconfig.read_text()
    kconfig_updated = require_mpc(kconfig_text)
    # Validate the entire upstream backport before changing any installed file.
    upstream_updates = prepare_backport_stack(qemu, root, packages=[
        ('emmc-9.2', 'eMMC'),
        ('tcg-iommu-2026', 'TCG IOMMU'),
        ('tcg-block-limit', 'TCG translation block limit'),
        ('armv7m-dwt', 'ARMv7M DWT'),
        ('armv7m-cache-maintenance', 'ARMv7M cache maintenance'),
        ('sd-read-ahead', 'SD/eMMC host read-ahead'),
        ('sd-media-state', 'SD/eMMC media state'),
    ])
    for destination, content in upstream_updates:
        destination.write_bytes(content)
    # QEMU 8.2 integration uses flat translation-unit names. The manifest owns
    # source locations; standalone tests compile the actual layered files.
    for name, source in selected.items():
        destination = directory / name
        content = source.read_bytes()
        if not destination.exists() or destination.read_bytes() != content:
            destination.write_bytes(content)
    generated = {'best1503_package_config.h': package_header,
                 'bes_runtime_decode.inc': decoder_source()}
    for name, content in generated.items():
        destination = directory / name
        if not destination.exists() or destination.read_bytes() != content:
            destination.write_bytes(content)
    # Remove obsolete product tables from an existing integration checkout.
    for name in ('miband_board_defaults.h', 'miband_boot_defaults.h',
                 'miband_audio_defaults.h', 'miband_binding_ui_defaults.h',
                 'miband_binding_auth_defaults.h'):
        (directory / name).unlink(missing_ok=True)
    if updated != text:
        meson.write_text(updated, newline='\n')
    if kconfig_updated != kconfig_text:
        kconfig.write_text(kconfig_updated, newline='\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('qemu', type=Path)
    args = parser.parse_args()
    install(args.qemu)
