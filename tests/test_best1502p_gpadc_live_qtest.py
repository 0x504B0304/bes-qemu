"""Runtime analog input changes reach future GPADC samples on native hosts."""
import os
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


@unittest.skipUnless(os.environ.get('MIBAND_TEST_QEMU'), 'MIBAND_TEST_QEMU is not set')
class LiveAdc(unittest.TestCase):
    def setUp(self):
        self.q = QtestMachine(os.environ['MIBAND_TEST_QEMU'],
            machine_name='miband-best1502p', machine_options=',gpadc-raw4=16608',
            serial_backends=('none', 'none'))
        self.addCleanup(self.q.close)
        self.q.write32(0x40009004, 0x180)

    def write(self, reg, value):
        self.q.write32(0x40009008, (reg << 18) | (value << 2))
        self.q.read32(0x40009008)

    def read(self, reg):
        self.q.write32(0x40009008, (1 << 26) | (reg << 18))
        return self.q.read32(0x40009008) >> 2

    def analog(self, value):
        self.q.qmp('qom-set', {'path': '/machine', 'property': 'gpadc-raw4', 'value': value})

    def test_elapsed_conversion_keeps_old_sample_then_next_uses_new_input(self):
        self.write(0x1d, 0x10)
        self.write(0x4f, 0x20)
        self.q.step(125000)
        self.analog(17000)
        self.assertEqual(self.read(0x5a), 16608)
        self.assertEqual(self.read(0x50), 0x110)
        self.write(0x51, 0x1ff)
        self.write(0x4f, 0x20)
        self.q.step(124999)
        self.assertEqual(self.read(0x50), 0)
        self.assertEqual(self.read(0x5a), 16608)
        self.q.step(1)
        self.assertEqual(self.read(0x5a), 17000)
        self.assertEqual(self.read(0x50), 0x110)
        self.assertEqual(self.read(0x58), 0, 'other channel must not acquire this input')
        self.q.qmp('system_reset')
        self.q.write32(0x40009004, 0x180)
        self.write(0x1d, 0x10)
        self.write(0x4f, 0x20)
        self.q.step(125000)
        self.assertEqual(self.read(0x5a), 17000)

    def test_invalid_input_is_rejected_and_disconnect_does_not_make_samples(self):
        with self.assertRaisesRegex(RuntimeError, '0..65535'):
            self.analog(65536)
        self.assertEqual(self.q.qmp('qom-get', {'path': '/machine', 'property': 'gpadc-raw4'}), 16608)
        self.analog(0xffffffff)
        self.write(0x1d, 0x10)
        self.write(0x4f, 0x20)
        self.q.step(125000)
        self.assertEqual(self.read(0x50), 0)
        self.assertEqual(self.read(0x5a), 0)


if __name__ == '__main__':
    unittest.main()
