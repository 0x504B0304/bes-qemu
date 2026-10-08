"""Integration must preserve unrelated QEMU boards and reject ambiguous input."""
import json
from pathlib import Path
import tempfile
import unittest
import shutil

from tools.integrate_qemu import install, require_mpc
from tools.source_layout import ROOT, sources
from tests.backport_fixture import backport_fixture


class Integration(unittest.TestCase):
    def test_no_test_sources_at_repository_root(self):
        misplaced = sorted(p.name for p in ROOT.iterdir()
                           if p.is_file() and (p.name == "bt_qtest.py" or
                           (p.name.startswith("test_") and
                            p.suffix in (".py", ".c", ".sh", ".mjs"))))
        self.assertEqual(misplaced, [], "Place reusable tests and helpers in tests/")

    def test_root_contains_only_repository_entry_files(self):
        misplaced = sorted(p.name for p in ROOT.iterdir() if p.is_file()
                           and p.suffix in (".py", ".c", ".h", ".sh", ".gdb", ".mjs", ".md")
                           and p.name not in ("build.sh", "README.md"))
        self.assertEqual(misplaced, [], "Place tools, hooks and documentation in their owning directories")

    def fixture(self, directory, text):
        base = Path(directory)
        root = base/'repo'
        (root/'qemu/machine').mkdir(parents=True)
        (root/'qemu/soc/best1503/package').mkdir(parents=True)
        shutil.copytree(ROOT/'qemu/soc/best1503/package/variants', root/'qemu/soc/best1503/package/variants')
        shutil.copyfile(ROOT/'qemu/soc/best1503/config.json', root/'qemu/soc/best1503/config.json')
        (root/'qemu/machine/miband.c').write_text('/* machine */\n')
        (root/'qemu/soc/core.c').write_text('/* soc */\n')
        (root/'qemu/machine/sources.json').write_text(json.dumps({
            'schema': 'bes.sources.v1', 'sources': ['qemu/machine/miband.c', 'qemu/soc/core.c']}))
        qemu = base/'qemu'
        (qemu/'hw/arm').mkdir(parents=True)
        (qemu/'hw/arm/meson.build').write_text(text)
        (qemu/'hw/arm/Kconfig').write_text(
            'config ARM_V7M\n    bool\n    select PTIMER\n\n'
            'config OTHER\n    bool\n')
        backport_fixture(root, qemu)
        backport_fixture(root, qemu, {
            'accel/tcg/cputlb.c': (b'/* section index */\n', b'/* section pointer */\n')},
            package_name='tcg-iommu-2026')
        backport_fixture(root, qemu, {
            'accel/tcg/translate-all.c': (b'/* fixed blocks */\n', b'/* bounded blocks */\n')},
            package_name='tcg-block-limit')
        backport_fixture(root, qemu, {
            'hw/arm/armv7m.c': (b'/* no DWT */\n', b'/* DWT */\n')},
            package_name='armv7m-dwt')
        backport_fixture(root, qemu, {
            'accel/tcg/cputlb.c': (b'/* section pointer */\n', b'/* section pointer, cache NOP */\n')},
            package_name='armv7m-cache-maintenance')
        backport_fixture(root, qemu, {
            'hw/sd/sd.c': (b'/* SD/MMC */\n', b'/* SD/MMC read-ahead */\n')},
            package_name='sd-read-ahead')
        backport_fixture(root, qemu, {
            'hw/sd/sd.c': (b'/* SD/MMC read-ahead */\n', b'/* SD/MMC read-ahead, media events */\n')},
            package_name='sd-media-state')
        (qemu / 'hw/sd/sd.c').write_bytes(b'/* SD */\n')
        (qemu / 'accel/tcg/cputlb.c').write_bytes(b'/* section index */\n')
        return root, qemu

    def test_pristine_and_existing_integration_are_idempotent(self):
        anchor = "arm_ss.add(when: 'CONFIG_ARM_V7M', if_true: files('armv7m.c'))\n"
        other = "arm_ss.add(when: 'CONFIG_OTHER', if_true: files('other.c'))\n"
        for old in ('', "arm_ss.add(when: 'CONFIG_ARM_V7M',\n if_true: files('miband.c', 'miband_spidpd.c'))\n"):
            with tempfile.TemporaryDirectory() as temp:
                root, qemu = self.fixture(temp, anchor+old+other)
                install(qemu, root)
                first = (qemu/'hw/arm/meson.build').read_bytes()
                first_config = (qemu/'hw/arm/Kconfig').read_bytes()
                install(qemu, root)
                self.assertEqual(first, (qemu/'hw/arm/meson.build').read_bytes())
                self.assertIn(other.encode(), first)
                self.assertEqual(first.count(b"'miband.c'"), 1)
                self.assertNotIn(b'miband_spidpd', first)
                self.assertEqual((qemu/'hw/arm/core.c').read_text(), '/* soc */\n')
                self.assertEqual((qemu/'hw/arm/Kconfig').read_bytes(), first_config)
                self.assertEqual(first_config.count(b'select TZ_MPC'), 1)
                self.assertIn(b'config OTHER\n    bool\n', first_config)

    def test_mpc_dependency_rejects_unknown_or_duplicate_owner(self):
        for config in ('config OTHER\n    bool\n',
                       'config ARM_V7M\n    int\n',
                       'config ARM_V7M\n    bool\nconfig ARM_V7M\n    bool\n'):
            with self.subTest(config=config), self.assertRaises(ValueError):
                require_mpc(config)

    def test_duplicate_and_escape_paths_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root, _ = self.fixture(temp, '')
            manifest = root/'qemu/machine/sources.json'
            for paths in (['qemu/machine/miband.c']*2, ['../qemu/hw/arm/meson.build']):
                manifest.write_text(json.dumps({'schema': 'bes.sources.v1', 'sources': paths}))
                with self.assertRaises(ValueError):
                    sources(root)

    def test_unknown_meson_layout_is_left_untouched(self):
        with tempfile.TemporaryDirectory() as temp:
            root, qemu = self.fixture(temp, '# unknown\n')
            with self.assertRaises(ValueError):
                install(qemu, root)
            self.assertEqual((qemu/'hw/arm/meson.build').read_text(), '# unknown\n')

    def test_upstream_conflict_prevents_owned_source_writes(self):
        with tempfile.TemporaryDirectory() as temp:
            anchor = "arm_ss.add(when: 'CONFIG_ARM_V7M', if_true: files('armv7m.c'))\n"
            root, qemu = self.fixture(temp, anchor)
            (qemu/'hw/sd/sd.c').write_text('/* local edit */\n')
            with self.assertRaisesRegex(ValueError, 'backport conflict'):
                install(qemu, root)
            self.assertFalse((qemu/'hw/arm/miband.c').exists())
            self.assertEqual((qemu/'hw/arm/meson.build').read_text(), anchor)

    def test_iommu_conflict_prevents_emmc_and_owned_source_writes(self):
        with tempfile.TemporaryDirectory() as temp:
            anchor = "arm_ss.add(when: 'CONFIG_ARM_V7M', if_true: files('armv7m.c'))\n"
            root, qemu = self.fixture(temp, anchor)
            original_config = (qemu/'hw/arm/Kconfig').read_bytes()
            (qemu/'accel/tcg/cputlb.c').write_text('/* local edit */\n')
            with self.assertRaisesRegex(ValueError, 'TCG IOMMU backport conflict'):
                install(qemu, root)
            self.assertEqual((qemu/'hw/sd/sd.c').read_bytes(), b'/* SD */\n')
            self.assertFalse((qemu/'hw/arm/miband.c').exists())
            self.assertEqual((qemu/'hw/arm/meson.build').read_text(), anchor)
            self.assertEqual((qemu/'hw/arm/Kconfig').read_bytes(), original_config)


if __name__ == '__main__':
    unittest.main()
