"""Limited board load-switch model through real BEST1700 I2C transactions."""
import os
import unittest

from tests.test_best1700_i2c_qtest import (
    I2cTestCase, I2C2, I2C_BASES, DATA_CMD, ABRT_SOURCE, RXFLR,
    CLR_INTR, CMD_READ, CMD_STOP, CMD_RESTART,
)
from tests.test_best1700_peripherals_qtest import QtestMachine


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Aw35206Qtest(I2cTestCase):
    def setUp(self):
        self.machine = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
            extra_args=('-device',
                        'aw35206,id=test-power,bus=/best1700-i2c2/i2c,address=0x18'))
        self.addCleanup(self.machine.close)
        self.configure(target=0x18)

    def write_register(self, register, value, *, accepted=True):
        q = self.machine
        q.read32(I2C2 + CLR_INTR)
        q.write32(I2C2 + DATA_CMD, register)
        q.write32(I2C2 + DATA_CMD, value | CMD_STOP)
        self.assertEqual(q.read32(I2C2 + ABRT_SOURCE), 0 if accepted else 8)
        q.read32(I2C2 + CLR_INTR)

    def read_register(self, register, *, selector_stop=False):
        q = self.machine
        q.read32(I2C2 + CLR_INTR)
        q.write32(I2C2 + DATA_CMD, register | (CMD_STOP if selector_stop else 0))
        q.write32(I2C2 + DATA_CMD, CMD_READ | CMD_RESTART | CMD_STOP)
        self.assertEqual(q.read32(I2C2 + ABRT_SOURCE), 0)
        self.assertEqual(q.read32(I2C2 + RXFLR), 1)
        return q.read32(I2C2 + DATA_CMD)

    def test_identity_is_read_only_and_selector_survives_repeated_start(self):
        self.assertEqual(self.read_register(0), 0x32)
        self.write_register(0, 0x20)
        self.assertEqual(self.read_register(0), 0x32)
        self.write_register(2, 0x25)
        self.assertEqual(self.read_register(0), 0x32)
        self.assertEqual(self.read_register(2), 0x25)
        self.assertEqual(self.read_register(2, selector_stop=True), 0x25)

    def test_probe_reset_and_six_channel_read_modify_write(self):
        self.assertEqual(self.read_register(0), 0x32)
        self.write_register(0x69, 0xae)
        self.assertEqual(self.read_register(2), 0)
        for channel in (1, 2, 4, 5, 6):
            value = self.read_register(2) | (1 << (channel - 1))
            self.write_register(2, value)
            self.assertEqual(self.read_register(2), value)
        self.assertEqual(self.read_register(2), 0x3b)
        self.write_register(2, self.read_register(2) & ~0x20)
        self.assertEqual(self.read_register(2), 0x1b,
                         'disabling channel 6 must retain other channels')
        self.write_register(2, self.read_register(2) | 0x20)
        self.assertEqual(self.read_register(2), 0x3b)
        self.write_register(2, 0xff)
        self.assertEqual(self.read_register(2), 0x3f)
        self.write_register(0x69, 0xae)
        self.assertEqual(self.read_register(2), 0)
        self.assertEqual(self.read_register(0), 0x32)

    def test_unsupported_registers_keys_and_bursts_do_not_alias_enables(self):
        q = self.machine
        self.write_register(2, 0x15)
        self.write_register(0x69, 0xb0, accepted=False)
        self.write_register(3, 0xff, accepted=False)
        self.write_register(0xff, 0xff, accepted=False)
        self.assertEqual(self.read_register(3), 0xff)
        self.assertEqual(self.read_register(2), 0x15)
        # Each accepted byte remains committed; an unsupported extra byte
        # is NACKed instead of being silently interpreted as another register.
        for word in (2, 0x2a, 0x3f | CMD_STOP):
            q.write32(I2C2 + DATA_CMD, word)
        self.assertEqual(q.read32(I2C2 + ABRT_SOURCE), 8)
        q.read32(I2C2 + CLR_INTR)
        self.assertEqual(self.read_register(2), 0x2a)
        q.write32(I2C2 + DATA_CMD, 0)
        q.write32(I2C2 + DATA_CMD, CMD_READ | CMD_RESTART)
        q.write32(I2C2 + DATA_CMD, CMD_READ | CMD_STOP)
        self.assertEqual(q.read32(I2C2 + RXFLR), 2)
        self.assertEqual(q.read32(I2C2 + DATA_CMD), 0x32)
        self.assertEqual(q.read32(I2C2 + DATA_CMD), 0xff)
        self.assertEqual(self.read_register(2), 0x2a)

    def test_attachment_is_specific_to_bus_and_address(self):
        q = self.machine
        for base, target in ((I2C2, 0x19), (I2C_BASES[1], 0x18)):
            self.configure(base=base, target=target)
            q.write32(base + DATA_CMD, CMD_READ | CMD_STOP)
            self.assertEqual(q.read32(base + ABRT_SOURCE), 1)
            self.assertEqual(q.read32(base + RXFLR), 0)
        self.configure(target=0x18)
        self.assertEqual(self.read_register(0), 0x32)

    def test_machine_reset_restores_initial_state_and_parser(self):
        q = self.machine
        self.write_register(2, 0x3f)
        q.write32(I2C2 + DATA_CMD, 0x69)
        q.qmp('system_reset')
        q.qmp('query-status')
        self.configure(target=0x18)
        self.assertEqual(self.read_register(0), 0x32)
        self.assertEqual(self.read_register(2), 0)
        self.write_register(2, 0x04)
        self.assertEqual(self.read_register(2), 0x04)


if __name__ == '__main__':
    unittest.main()
