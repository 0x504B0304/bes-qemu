"""Configurable battery candidates through real BEST1700 I2C bus transactions."""
import os
import unittest

from tests.test_best1700_i2c_qtest import (
    I2cTestCase, I2C_BASES, DATA_CMD, ABRT_SOURCE, RXFLR, CLR_INTR,
    CMD_READ, CMD_STOP, CMD_RESTART,
)
from tests.test_best1700_peripherals_qtest import QtestMachine


I2C0 = I2C_BASES[0]
GAUGE = 0x64
CHARGER = 0x6b


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class SystemPowerQtest(I2cTestCase):
    def setUp(self):
        self.machine = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'], extra_args=(
            '-device', 'cw221x,id=gauge,bus=/best1700-i2c0/i2c,address=0x64,'
                       'voltage-mv=4100,capacity-pct=73,temperature-decic=255',
            '-device', 'sc89620,id=charger,bus=/best1700-i2c0/i2c,address=0x6b,'
                       'voltage-mv=4100,online=on'))
        self.addCleanup(self.machine.close)

    def write_registers(self, address, register, data, *, accepted=True):
        q = self.machine
        self.configure(target=address, base=I2C0)
        q.read32(I2C0 + CLR_INTR)
        q.write32(I2C0 + DATA_CMD, register)
        for i, byte in enumerate(data):
            q.write32(I2C0 + DATA_CMD, byte | (CMD_STOP if i == len(data) - 1 else 0))
        self.assertEqual(q.read32(I2C0 + ABRT_SOURCE), 0 if accepted else 8)
        q.read32(I2C0 + CLR_INTR)

    def read_registers(self, address, register, size=1, *, selector_stop=False):
        q = self.machine
        self.configure(target=address, base=I2C0)
        q.read32(I2C0 + CLR_INTR)
        q.write32(I2C0 + DATA_CMD, register | (CMD_STOP if selector_stop else 0))
        result = []
        for i in range(size):
            q.write32(I2C0 + DATA_CMD, CMD_READ | (CMD_RESTART if i == 0 else 0) |
                      (CMD_STOP if i == size - 1 else 0))
            self.assertEqual(q.read32(I2C0 + ABRT_SOURCE), 0)
            self.assertEqual(q.read32(I2C0 + RXFLR), 1)
            result.append(q.read32(I2C0 + DATA_CMD))
        return bytes(result)

    def activate_gauge(self):
        # Exercise the actual driver ordering: sleep, profile, update flag,
        # reset/active, then poll ready. Profile data is synthetic test input.
        self.write_registers(GAUGE, 8, [0x30])
        self.write_registers(GAUGE, 8, [0xf0])
        profile = bytearray(range(80))
        profile[64:67] = bytes([0, 0xa0, 96])  # cycle 10, SOH 96%
        self.write_registers(GAUGE, 0x10, profile)
        self.write_registers(GAUGE, 0x0b, [0x80])
        self.write_registers(GAUGE, 0x0a, [0])
        self.assertEqual(self.read_registers(GAUGE, 0xa7), b'\x00')
        self.write_registers(GAUGE, 8, [0x30])
        self.write_registers(GAUGE, 8, [0])
        self.assertEqual(self.read_registers(GAUGE, 0xa7), b'\x0c')
        return bytes(profile)

    def test_gauge_uploads_guest_profile_before_becoming_ready(self):
        self.assertEqual(self.read_registers(GAUGE, 8), b'\xf0')
        self.assertEqual(self.read_registers(GAUGE, 0xa7), b'\x00')
        self.assertEqual(self.read_registers(GAUGE, 4, 2), b'\x00\x00')
        profile = self.activate_gauge()
        self.assertEqual(self.read_registers(GAUGE, 0x10, 80), profile)
        self.assertEqual(self.read_registers(GAUGE, 0xa4, 3), b'\x00\xa0\x60')
        self.assertEqual(self.read_registers(GAUGE, 8, selector_stop=True), b'\x00')
        self.write_registers(GAUGE, 8, [0xf0])
        self.assertEqual(self.read_registers(GAUGE, 0xa7), b'\x00')
        self.assertEqual(self.read_registers(GAUGE, 0x10, 80), profile)
        self.write_registers(GAUGE, 8, [0])
        self.assertEqual(self.read_registers(GAUGE, 0xa7), b'\x0c')

    def test_battery_measurements_use_guest_units_and_big_endian_words(self):
        self.activate_gauge()
        values = self.read_registers(GAUGE, 2, 5)
        self.assertEqual(int.from_bytes(values[:2], 'big') * 5 // 16, 4100)
        self.assertEqual(int.from_bytes(values[2:4], 'big') // 256, 73)
        self.assertEqual(values[4] * 5 - 400, 255)
        self.assertEqual(self.read_registers(GAUGE, 0x0e, 2), b'\x00\x00')
        self.write_registers(GAUGE, 2, [0, 0, 0, 0, 0])
        self.assertEqual(self.read_registers(GAUGE, 2, 5), values)
        self.write_registers(GAUGE, 0xa7, [0])
        self.assertEqual(self.read_registers(GAUGE, 0xa7), b'\x0c')

    def test_charger_probe_reset_configuration_and_adc(self):
        self.assertEqual(self.read_registers(CHARGER, 0x38), b'\x58')
        self.write_registers(CHARGER, 0x38, [0])
        self.assertEqual(self.read_registers(CHARGER, 0x38), b'\x58')
        self.write_registers(CHARGER, 0x17, [0x80])
        self.assertEqual(self.read_registers(CHARGER, 0x17), b'\x00')
        # Firmware's extended configuration must retain read-modify-writes.
        self.write_registers(CHARGER, 0x90, [8, 0x5d, 0x40])
        self.assertEqual(self.read_registers(CHARGER, 0x90, 3), b'\x08\x5d\x40')
        self.assertEqual(self.read_registers(CHARGER, 0x1e), b'\x40')
        self.write_registers(CHARGER, 0x16, [0x20])
        status = self.read_registers(CHARGER, 0x1e)[0]
        self.assertEqual(status & 0x40, 0x40)
        self.assertIn((status >> 3) & 7, (1, 3, 4, 5))
        adc = self.read_registers(CHARGER, 0x29)[0]
        self.assertLessEqual(abs(18 * (adc + 1) - 4100), 17)
        self.write_registers(CHARGER, 0x1e, [0])
        self.assertEqual(self.read_registers(CHARGER, 0x1e)[0], status)
        self.write_registers(CHARGER, 0x17, [0x80])
        self.assertEqual(self.read_registers(CHARGER, 0x16), b'\x00')
        self.assertEqual(self.read_registers(CHARGER, 0x1e), b'\x40')
        self.assertEqual(self.read_registers(CHARGER, 0x38), b'\x58')

    def test_unsupported_accesses_do_not_silently_alias_control_registers(self):
        self.activate_gauge()
        self.write_registers(GAUGE, 8, [1], accepted=False)
        self.assertEqual(self.read_registers(GAUGE, 8), b'\x00')
        for device in (GAUGE, CHARGER):
            self.write_registers(device, 0xf8, [0x66], accepted=False)
            self.assertEqual(self.read_registers(device, 0xf8), b'\xff')
        # No exact CW221X identity has been established from this firmware.
        self.assertEqual(self.read_registers(GAUGE, 0), b'\xff')

    def test_host_battery_update_reaches_i2c_and_preserves_guest_control(self):
        self.activate_gauge()
        self.write_registers(CHARGER, 0x16, [0x20])
        q = self.machine
        self.assertEqual(q.qmp('qom-get', {'path': '/machine', 'property': 'charger-state'}),
                         {'level': 73, 'charging': True})
        for level, online in ((25, False), (86, True), (100, False)):
            value = {'level': level, 'charging': online}
            self.assertEqual(q.qmp('qom-set', {'path': '/machine',
                'property': 'charger-state', 'value': value}), {})
            self.assertEqual(int.from_bytes(self.read_registers(GAUGE, 4, 2), 'big') // 256, level)
            self.assertEqual(self.read_registers(CHARGER, 0x1e)[0], 0x48 if online else 0)
            self.assertEqual(self.read_registers(CHARGER, 0x16), b'\x20')
            self.assertEqual(self.read_registers(GAUGE, 0xa7), b'\x0c')
        with self.assertRaises(RuntimeError):
            q.qmp('qom-set', {'path': '/machine', 'property': 'charger-state',
                              'value': {'level': 101, 'charging': True}})
        self.assertEqual(q.qmp('qom-get', {'path': '/machine', 'property': 'charger-state'}),
                         {'level': 100, 'charging': False})

    def test_slave_selection_and_reset_preserve_board_battery_environment(self):
        self.activate_gauge()
        q = self.machine
        for base, target in ((I2C0, 0x65), (I2C_BASES[1], GAUGE)):
            self.configure(base=base, target=target)
            q.write32(base + DATA_CMD, CMD_READ | CMD_STOP)
            self.assertEqual(q.read32(base + ABRT_SOURCE), 1)
            self.assertEqual(q.read32(base + RXFLR), 0)
        self.configure(base=I2C0, target=GAUGE)
        q.write32(I2C0 + DATA_CMD, 0x10)
        q.qmp('system_reset')
        q.qmp('query-status')
        self.assertEqual(self.read_registers(GAUGE, 8), b'\xf0')
        self.assertEqual(self.read_registers(GAUGE, 0xa7), b'\x00')
        self.assertEqual(self.read_registers(GAUGE, 0x10), b'\x00')
        self.assertEqual(self.read_registers(CHARGER, 0x38), b'\x58')
        self.assertEqual(self.read_registers(CHARGER, 0x1e), b'\x40')
        self.activate_gauge()
        self.assertEqual(self.read_registers(GAUGE, 4, 2), b'\x49\x00')


if __name__ == '__main__':
    unittest.main()
