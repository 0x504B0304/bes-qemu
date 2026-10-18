"""Firmware-independent charger bus identity and cable-state regression."""
import os
import struct
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


@unittest.skipUnless(os.environ.get('MIBAND_TEST_QEMU'), 'MIBAND_TEST_QEMU is not set')
class ChargerIdentity(unittest.TestCase):
    def start(self, device_id=None):
        options = ',charger-model=on,charger-i2c-bus=0,charger-probe-revision=6'
        # This machine realizes external I2C devices alongside its display.
        options += ',display-width=16,display-height=16'
        if device_id is not None:
            options += f',charger-device-id={device_id}'
        self.q = QtestMachine(os.environ['MIBAND_TEST_QEMU'],
            machine_name='miband-best1502p', machine_options=options,
            image_payload=struct.pack('<II', 0x20010000, 0x0c0c0009) + b'\xfe\xe7' + bytes(4096),
            serial_backends=('none', 'none'))
        self.addCleanup(self.q.close)
        self.q.write32(0x40005004, 7)
        self.q.write32(0x4000506c, 1)

    def read_reg(self, reg):
        self.q.write32(0x40005010, reg | 0x400)
        self.q.write32(0x40005010, 0x300)
        self.q.command('clock_step 22500')
        self.q.command('clock_step 22500')
        return self.q.read32(0x40005010)

    def write_reg(self, reg, value):
        self.q.write32(0x40005010, reg | 0x400)
        self.q.write32(0x40005010, value | 0x200)
        self.q.command('clock_step 22500')
        self.q.command('clock_step 22500')

    def test_default_identity(self):
        self.start()
        self.assertEqual(self.read_reg(0), 0x1e)
        self.assertEqual(self.read_reg(1), 6)

        self.assertIs(self.q.qmp('qom-get', {'path': '/machine', 'property': 'charger-state'})['level-adjustable'], False)

    def pmu_write(self, reg, value):
        self.q.write32(0x40009008, (reg << 18) | (value << 2))
        self.q.read32(0x40009008)

    def pmu_read(self, reg):
        self.q.write32(0x40009008, (1 << 26) | (reg << 18))
        return self.q.read32(0x40009008) >> 2

    def cable(self, connected):
        self.q.qmp('qom-set', {'path': '/machine', 'property': 'charger-state',
                              'value': {'level': 75, 'charging': connected}})

    def test_pmu_edges_enable_masks_w1c_and_reset(self):
        self.start(0xe0)
        self.q.command('irq_intercept_in /machine/armv7m')
        self.q.write32(0x40009004, 0x180)
        self.cable(True)
        self.assertEqual(self.pmu_read(0x5e), 0x0c)
        self.assertFalse(self.q.irq(42), 'disabled edge must remain latched without IRQ')
        self.pmu_write(0x05, 0x1800)
        self.assertFalse(self.q.irq(42), 'unified slot must also be enabled')
        self.pmu_write(0x40, 0x1000)
        self.assertTrue(self.q.irq(42))
        self.pmu_write(0x5e, 0xffff)
        self.assertEqual(self.pmu_read(0x5e), 4, 'ACK must not clear cable presence')
        self.assertFalse(self.q.irq(42))
        self.cable(True)
        self.assertEqual(self.pmu_read(0x5e), 4, 'same input must not synthesize an edge')
        self.cable(False)
        self.assertEqual(self.pmu_read(0x5e), 0x10)
        self.assertTrue(self.q.irq(42))
        self.pmu_write(0x05, 0x800)
        self.assertFalse(self.q.irq(42), 'removal has a separate enable bit')
        self.pmu_write(0x05, 0x1800)
        self.assertTrue(self.q.irq(42))
        self.pmu_write(0x5e, 0x10)
        self.assertFalse(self.q.irq(42))
        self.cable(True)
        self.assertTrue(self.q.irq(42))
        self.q.qmp('system_reset')
        self.q.write32(0x40009004, 0x180)
        self.assertEqual(self.pmu_read(0x5e), 4)
        self.assertFalse(self.q.irq(42), 'reset must clear latches and deassert the old line')
        self.pmu_write(0x05, 0x1800)
        self.pmu_write(0x40, 0x1000)
        self.cable(False)
        self.assertTrue(self.q.irq(42), 'reset must retain IRQ wiring')
        self.pmu_write(0x5e, 0x10)
        self.pmu_write(0x5e, 4)
        self.assertEqual(self.pmu_read(0x5e), 0, 'guest cannot set cable input')

    def test_configured_identity_and_cable_survive_reset(self):
        self.start(0xe0)
        self.assertEqual(self.read_reg(0), 0xe0)
        self.write_reg(0, 0x55)
        self.write_reg(1, 0x55)
        self.assertEqual(self.read_reg(0), 0xe0)
        self.assertEqual(self.read_reg(1), 6)
        self.q.qmp('qom-set', {'path': '/machine', 'property': 'charger-state',
                              'value': {'level': 75, 'charging': True}})
        self.assertEqual(self.read_reg(0x30), 0x28)
        self.write_reg(0x1d, 0x40)
        self.assertEqual(self.read_reg(0x30), 8)
        self.q.qmp('system_reset')
        self.q.write32(0x40005004, 7)
        self.q.write32(0x4000506c, 1)
        self.assertEqual(self.read_reg(0), 0xe0)
        self.assertEqual(self.read_reg(1), 6)
        self.assertEqual(self.read_reg(0x30), 0x28)
        with self.assertRaisesRegex(RuntimeError, 'startup-only'):
            self.q.qmp('qom-set', {'path': '/machine', 'property': 'charger-device-id', 'value': 0x1e})


if __name__ == '__main__':
    unittest.main()
