"""Verify guest I2C haptic writes reach the QOM bridge on both AP machines."""
import os
import struct
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


@unittest.skipUnless(os.environ.get('MIBAND_BEST1502P_QEMU'), 'native QEMU not set')
class ApHapticsTest(unittest.TestCase):
    def test_haptic_slave_on_ap_buses(self):
        for machine, index, base in (('bes1503', 2, 0x40060000),
                                     ('miband-best1502p', 4, 0x40062000)):
            with self.subTest(machine=machine):
                q = QtestMachine(os.environ['MIBAND_BEST1502P_QEMU'], machine_name=machine,
                    image_payload=struct.pack('<II', 0x20010000, 0x0c0c0009) + b'\xfe\xe7' + bytes(4096),
                    extra_args=('-device', f'aw86225,id=board-haptic,bus=miband-ap-i2c{index},address=0x58'),
                    serial_backends=['none', 'none'])
                try:
                    self.exercise_haptic(q, base)
                finally:
                    q.close()

    def exercise_haptic(self, q, base):
        q.write32(base + 4, 0x58)
        q.write32(base + 0x6c, 1)

        def transfer(*commands):
            for command in commands:
                q.write32(base + 0x10, command)
                q.step(100_000)
            self.assertEqual(q.read32(base + 0x80), 0)

        def prop(name):
            return q.qmp('qom-get', {'path': '/machine/peripheral/board-haptic', 'property': name})

        transfer(0, 0x700)  # Register address, repeated start, read, stop.
        self.assertEqual(q.read32(base + 0x78), 1)
        self.assertEqual(q.read32(base + 0x10), 0)
        transfer(8, 0x202)  # Continuous mode.
        transfer(0x18, 0x208)
        transfer(9, 0x201)  # Calibrate without a host vibration event.
        self.assertEqual(prop('plays'), 0)
        q.step(100_000_000)
        transfer(0x18, 0x200)
        transfer(9, 0x201)
        self.assertEqual(prop('plays'), 1)
        self.assertGreater(prop('last-duration-ns'), 0)
        transfer(9, 0x202)
        transfer(0x3f, 0x700)
        self.assertEqual(q.read32(base + 0x10), 0)
