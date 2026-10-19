"""Synthetic BTH NOR-window persistence, without any product firmware."""
import hashlib
import os
from pathlib import Path
import struct
import tempfile
import unittest

from tests.test_best1700_bth_flash_qtest import FlashPort, BTH, OPTIONS, ALIASES
from tests.test_best1700_peripherals_qtest import QtestMachine


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class BthNorPersistence(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='bth-nor-store-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.image = self.root / 'sensor.bin'
        self.image.write_bytes(b'\xfe\xe7' + bytes(254))
        self.state = self.root / 'sensor.journal'
        self.offset, self.size = 0x200000, 8192

    def machine(self):
        opts = (OPTIONS + f',bth-image={self.image},bth-nor-state={self.state},'
                f'bth-nor-state-offset={self.offset},bth-nor-state-size={self.size}')
        q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'], machine_options=opts)
        self.addCleanup(q.close)
        return q

    def test_program_erase_restart_and_window_isolation(self):
        q = self.machine()
        port = FlashPort(self, q, BTH)
        port.program(self.offset + 16, b'PERSIST!')
        port.program(self.offset - 4096, b'OUTSIDE!')
        for base in ALIASES:
            self.assertEqual(q.read(base + self.offset + 16, 8), b'PERSIST!')
        q.close()
        q = self.machine()
        for base in ALIASES:
            self.assertEqual(q.read(base + self.offset + 16, 8), b'PERSIST!')
            self.assertEqual(q.read(base + self.offset - 4096, 8), b'\xff' * 8)
        port = FlashPort(self, q, BTH)
        port.normal(0x06); port.normal(0x20, self.offset); q.step(200000)
        q.close()
        q = self.machine()
        self.assertEqual(q.read(ALIASES[0] + self.offset + 16, 8), b'\xff' * 8)
        q.close()
        header = self.state.read_bytes()[:64]
        self.assertEqual(struct.unpack_from('<3I', header, 8), (self.size, 0, 0))
        binding = hashlib.sha256(b'best1700-bth-nor-v1\0' + self.image.read_bytes() +
                                 struct.pack('<II', self.offset, self.size)).digest()
        self.assertEqual(header[32:64], binding)
        self.assertEqual(self.image.read_bytes(), b'\xfe\xe7' + bytes(254))

    def test_second_process_cannot_open_owned_state(self):
        q = self.machine(); q.read32(ALIASES[0] + self.offset)
        other = self.machine()
        with self.assertRaisesRegex(RuntimeError, 'state is locked'):
            other.read32(ALIASES[0] + self.offset)

    def test_changed_source_or_geometry_cannot_replay(self):
        q = self.machine(); q.read32(ALIASES[0] + self.offset); q.close()
        self.image.write_bytes(b'\x00\xbf' + bytes(254))
        q = self.machine()
        with self.assertRaisesRegex(RuntimeError, 'wrong base image'):
            q.read32(ALIASES[0] + self.offset)
        q.close()
        self.image.write_bytes(b'\xfe\xe7' + bytes(254))
        self.offset += 4096
        q = self.machine()
        with self.assertRaisesRegex(RuntimeError, 'wrong base image'):
            q.read32(ALIASES[0] + self.offset)


if __name__ == '__main__':
    unittest.main()
