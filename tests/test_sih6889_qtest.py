"""Exercise the Band 10 Pro SIH6889 through the guest's AP I2C bus."""
import os
import struct
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


@unittest.skipUnless(os.environ.get('MIBAND_BEST1502P_QEMU'), 'native QEMU not set')
class Sih6889Test(unittest.TestCase):
    def setUp(self):
        self.q = QtestMachine(os.environ['MIBAND_BEST1502P_QEMU'], machine_name='bes1503',
            image_payload=struct.pack('<II', 0x20010000, 0x0c0c0009) + b'\xfe\xe7' + bytes(4096),
            extra_args=('-device', 'sih6889,id=board-haptic,bus=miband-ap-i2c2,address=0x6b'),
            serial_backends=['none', 'none'])
        self.addCleanup(self.q.close)
        self.bus = 0x40060000
        self.q.write32(self.bus + 4, 0x6b)
        self.q.write32(self.bus + 0x6c, 1)

    def transfer(self, *commands):
        for command in commands:
            self.q.write32(self.bus + 0x10, command)
            self.q.step(100_000)
        self.assertEqual(self.q.read32(self.bus + 0x80), 0)

    def write(self, register, *data):
        self.transfer(register, *data[:-1], data[-1] | 0x200)

    def read(self, register):
        self.transfer(register, 0x700)
        return self.q.read32(self.bus + 0x10)

    def prop(self, name):
        return self.q.qmp('qom-get', {'path': '/machine/peripheral/board-haptic',
                                      'property': name})

    def test_ram_playback_calibration_and_stop(self):
        self.assertEqual(self.read(0), 0x89)
        self.write(0xfd, 0x89)
        self.write(0x10, 1)
        self.write(0x0f, 1)
        self.assertEqual(self.prop('plays'), 0)

        self.write(0x04, 0x80)
        self.write(0x20, 0x08, 0x00)
        self.write(0x28, 0x08, 0x00)
        self.write(0x2a, 0x01, 0x08, 0x05, 0x08, 0x64)
        self.write(0x28, 0x08, 0x01)
        self.assertEqual(self.read(0x2a), 8)
        self.write(0x04, 0)

        self.write(0x0f, 4)
        self.assertNotEqual(self.read(0x0e), 1)
        self.q.step(25_000_000)
        self.assertEqual(self.read(0x0e), 1)
        self.assertEqual(self.prop('plays'), 0)

        self.write(0x34, 2)
        self.write(0x5c, 2)
        self.write(0x0f, 1)
        self.assertEqual(self.prop('plays'), 1)
        self.assertEqual(self.prop('last-duration-ns'), 12_000_000)
        self.assertNotEqual(self.read(0x0e), 1)
        self.q.step(15_000_000)
        self.assertEqual(self.read(0x0e), 1)

        self.write(0x1c, 15)
        self.write(0x0f, 1)
        self.q.step(250_000_000)
        self.assertNotEqual(self.read(0x0e), 1)
        self.assertEqual(self.prop('plays'), 2)
        self.write(0x0f, 0x10)
        self.assertEqual(self.read(0x0e), 1)

    def test_rtp_requires_data_and_counts_a_single_start(self):
        self.write(0x0f, 2)
        self.assertEqual(self.prop('plays'), 0)
        self.write(0x2b, 15, 75, 90)
        self.assertEqual(self.prop('plays'), 1)
        self.write(0x2b, 88, 82, 66)
        self.assertEqual(self.prop('plays'), 1)
        self.write(0x0f, 0x10)
        self.assertEqual(self.read(0x0e), 1)
