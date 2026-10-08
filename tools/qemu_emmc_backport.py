"""Compatibility entry point for the pinned upstream SD/MMC backport."""
from tools.qemu_backport import _apply, _sections, prepare_backport
from tools.source_layout import ROOT


def prepare_emmc_backport(qemu, root=ROOT):
    return prepare_backport(qemu, root, package_name='emmc-9.2', label='eMMC')
