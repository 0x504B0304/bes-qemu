"""A pinned upstream backport must never overwrite unknown local changes."""
from hashlib import sha256
import json
from pathlib import Path
import tempfile
import unittest

from tests.backport_fixture import backport_fixture
from tools.qemu_emmc_backport import prepare_emmc_backport, _apply, _sections
from tools.source_layout import ROOT


class EmmcBackport(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        base = Path(self.temporary.name)
        self.root, self.qemu = base / 'repo', base / 'qemu'
        self.edits = {
            'hw/sd/sd.c': (b'/* SD */\nkeep\n', b'/* SD/MMC */\nkeep\nnew\n'),
            'include/hw/sd/sd.h': (b'old\n', b'new\n'),
        }
        self.package = backport_fixture(self.root, self.qemu, self.edits)

    def apply(self):
        updates = prepare_emmc_backport(self.qemu, self.root)
        for path, content in updates:
            path.write_bytes(content)
        return updates

    def test_pristine_install_and_repeat(self):
        self.assertEqual(len(self.apply()), 2)
        for name, (_, after) in self.edits.items():
            self.assertEqual((self.qemu / name).read_bytes(), after)
        self.assertEqual(self.apply(), [])

    def test_interrupted_install_can_be_completed(self):
        name = next(iter(self.edits))
        (self.qemu / name).write_bytes(self.edits[name][1])
        self.assertEqual(len(self.apply()), 1)
        self.assertEqual(self.apply(), [])

    def test_conflict_in_last_file_leaves_every_file_unchanged(self):
        target = self.qemu / 'include/hw/sd/sd.h'
        target.write_bytes(b'local edit\n')
        with self.assertRaisesRegex(ValueError, 'conflict.*include/hw/sd/sd.h'):
            self.apply()
        self.assertEqual(target.read_bytes(), b'local edit\n')
        self.assertEqual((self.qemu / 'hw/sd/sd.c').read_bytes(),
                         self.edits['hw/sd/sd.c'][0])

    def test_edits_after_install_are_not_overwritten(self):
        self.apply()
        path = self.qemu / 'hw/sd/sd.c'
        path.write_bytes(path.read_bytes() + b'local edit\n')
        with self.assertRaisesRegex(ValueError, 'conflict'):
            self.apply()
        self.assertTrue(path.read_bytes().endswith(b'local edit\n'))

    def test_wrong_version_and_missing_file_fail(self):
        (self.qemu / 'VERSION').write_bytes(b'9.2.0\n')
        with self.assertRaisesRegex(ValueError, 'requires QEMU 8.2.10'):
            self.apply()
        (self.qemu / 'VERSION').write_bytes(b'8.2.10\n')
        (self.qemu / 'include/hw/sd/sd.h').unlink()
        with self.assertRaises(FileNotFoundError):
            self.apply()
        self.assertEqual((self.qemu / 'hw/sd/sd.c').read_bytes(),
                         self.edits['hw/sd/sd.c'][0])

    def test_patch_tampering_and_wrong_result_hash_fail(self):
        patch = self.package / 'emmc.patch'
        original = patch.read_bytes()
        patch.write_bytes(original + b'bad\n')
        with self.assertRaisesRegex(ValueError, 'patch checksum mismatch'):
            self.apply()
        patch.write_bytes(original)
        manifest = self.package / 'manifest.json'
        data = json.loads(manifest.read_text())
        data['files'][-1]['after_sha256'] = '0' * 64
        manifest.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'result checksum mismatch'):
            self.apply()
        self.assertEqual((self.qemu / 'hw/sd/sd.c').read_bytes(),
                         self.edits['hw/sd/sd.c'][0])

    def test_path_escape_rejected(self):
        backport_fixture(self.root, self.qemu, {'../outside': (b'a\n', b'b\n')})
        with self.assertRaisesRegex(ValueError, 'unsafe backport path'):
            self.apply()
        self.assertEqual((self.qemu.parent / 'outside').read_bytes(), b'a\n')

    def test_hunks_do_not_fuzz_or_ignore_bad_counts(self):
        with self.assertRaisesRegex(ValueError, 'context mismatch'):
            _apply(b'actual\n', [b'@@ -1 +1 @@\n', b'-expected\n', b'+new\n'], 'test')
        with self.assertRaisesRegex(ValueError, 'hunk length'):
            _apply(b'old\n', [b'@@ -1,2 +1 @@\n', b'-old\n', b'+new\n'], 'test')

    def test_shipped_package_integrity_and_required_compatibility(self):
        package = ROOT / 'qemu/machine/patches/emmc-9.2'
        manifest = json.loads((package / 'manifest.json').read_text())
        payload = (package / manifest['patch']).read_bytes()
        self.assertEqual(sha256(payload).hexdigest(), manifest['patch_sha256'])
        self.assertEqual(set(_sections(payload)), {item['path'] for item in manifest['files']})
        self.assertEqual(len(manifest['files']), 8)
        self.assertIn(b'+    sd_spi = -2,', payload)
        self.assertNotIn(b'-system_ss.add(when: \'CONFIG_PXA2XX\'', payload)
        self.assertNotIn(b'-} sd_cmd_type_t;', payload)


if __name__ == '__main__':
    unittest.main()
