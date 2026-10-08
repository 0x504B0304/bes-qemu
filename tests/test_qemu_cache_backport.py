"""Overlapping pinned packages accept exact known stages and reject local edits."""
from pathlib import Path
import tempfile
import unittest
from tests.backport_fixture import backport_fixture
from tools.qemu_backport import prepare_backport_stack

class CacheBackportTests(unittest.TestCase):
    def test_fresh_partial_final_and_conflicting_installations(self):
        for state in [b'A\n', b'A\nB\n', b'A\nB\nC\n', b'local edit\n']:
            with self.subTest(state=state), tempfile.TemporaryDirectory() as t:
                root, qemu = Path(t)/'repo', Path(t)/'qemu'
                path = qemu/'shared.c'
                backport_fixture(root, qemu, {'shared.c': (b'A\n', b'A\nB\n')}, package_name='first')
                backport_fixture(root, qemu, {'shared.c': (b'A\nB\n', b'A\nB\nC\n')}, package_name='second')
                path.write_bytes(state)
                prepare = lambda: prepare_backport_stack(qemu, root, packages=[('first','First'),('second','Second')])
                if state == b'local edit\n':
                    with self.assertRaisesRegex(ValueError, 'conflict'):
                        prepare()
                    self.assertEqual(path.read_bytes(), state)
                else:
                    updates = prepare()
                    self.assertEqual(path.read_bytes(), state)
                    for dst, data in updates:
                        dst.write_bytes(data)
                    self.assertEqual(path.read_bytes(), b'A\nB\nC\n')
                    self.assertEqual(prepare(), [])

if __name__ == '__main__': unittest.main()
