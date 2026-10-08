"""The TCG IOMMU backport is pinned, repeatable, and rejects local edits."""
from hashlib import sha256
import json
from pathlib import Path
import tempfile
import unittest

from tests.backport_fixture import backport_fixture
from tools.qemu_backport import _sections, prepare_backport
from tools.source_layout import ROOT


class IommuBackport(unittest.TestCase):
    def test_install_repeat_and_conflict(self):
        with tempfile.TemporaryDirectory() as temp:
            root, qemu = Path(temp) / 'repo', Path(temp) / 'qemu'
            edits = {'accel/tcg/cputlb.c': (b'index\n', b'section\n'),
                     'include/hw/core/cpu.h': (b'offset\n', b'offset\npointer\n')}
            backport_fixture(root, qemu, edits, package_name='tcg-iommu-2026')
            prepare = lambda: prepare_backport(
                qemu, root, package_name='tcg-iommu-2026', label='TCG IOMMU')
            updates = prepare()
            self.assertEqual(len(updates), 2)
            for path, content in updates:
                path.write_bytes(content)
            self.assertEqual(prepare(), [])
            (qemu / 'accel/tcg/cputlb.c').write_bytes(b'local edit\n')
            with self.assertRaisesRegex(ValueError, 'TCG IOMMU backport conflict'):
                prepare()
            self.assertEqual((qemu / 'accel/tcg/cputlb.c').read_bytes(), b'local edit\n')

    def test_package_path_cannot_escape_patch_directory(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(ValueError, 'unsafe backport path'):
                prepare_backport(temp, temp, package_name='../outside')

    def test_shipped_upstream_fix_integrity(self):
        package = ROOT / 'qemu/machine/patches/tcg-iommu-2026'
        manifest = json.loads((package / 'manifest.json').read_text())
        payload = (package / manifest['patch']).read_bytes()
        self.assertEqual(sha256(payload).hexdigest(), manifest['patch_sha256'])
        self.assertEqual(set(_sections(payload)), {item['path'] for item in manifest['files']})
        self.assertEqual(len(manifest['files']), 4)
        self.assertIn('854cd16e318eed12de2995014b28d9f374c64bf7',
                      manifest['upstream_commits'])
        self.assertIn(b'+    full->section = section;', payload)
        self.assertIn(b'+    section = full->section;', payload)
        self.assertIn(b'-    section = iotlb_to_section(cpu, xlat, attrs);', payload)


if __name__ == '__main__':
    unittest.main()
