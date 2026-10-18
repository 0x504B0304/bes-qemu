"""BES AP I2C async IRQ, missing-slave failure and polling regressions.

Set MIBAND_BEST1502P_QEMU to a native build. Uses a synthetic AP only.
"""
import os
import struct
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine

BASES = (0x40005000, 0x40006000, 0x40060000, 0x40061000,
         0x40062000, 0x40063000, 0x40064000)
IRQS = (14, 29, 64, 65, 53, 54, 55)
TX_EMPTY, RX_FULL, ABORT, STOP = 16, 4, 64, 512


@unittest.skipUnless(os.environ.get('MIBAND_BEST1502P_QEMU'), 'native QEMU not set')
class Best1502pI2cTest(unittest.TestCase):
    machine_options = ''

    def setUp(self):
        self.q = QtestMachine(os.environ['MIBAND_BEST1502P_QEMU'],
            machine_name='miband-best1502p',
            machine_options=self.machine_options,
            image_payload=struct.pack('<II', 0x20010000, 0x0c0c0009)+b'\xfe\xe7'+bytes(4096),
            serial_backends=['none', 'none'])
        self.addCleanup(self.q.close)
        self.q.command('irq_intercept_in /machine/armv7m')

    def test_empty_fifo_irq_mask_clear_disable_and_controller_isolation(self):
        q = self.q
        for base, irq in zip(BASES, IRQS):
            q.write32(base+0x30, TX_EMPTY)
            self.assertFalse(q.irq(irq))
            q.write32(base+0x6c, 1)
            self.assertTrue(q.irq(irq))
            self.assertEqual(q.read32(base+0x34), TX_EMPTY)
            q.read32(base+0x40)
            self.assertTrue(q.irq(irq), 'CLR_INTR cannot clear a FIFO level')
            self.assertEqual(q.read32(base+0x34), TX_EMPTY)
            for peer in IRQS:
                self.assertEqual(q.irq(peer), peer == irq)
            q.write32(base+0x30, 0)
            self.assertFalse(q.irq(irq))
            q.write32(base+0x30, TX_EMPTY)
            q.write32(base+0x6c, 0)
            self.assertFalse(q.irq(irq))
            self.assertEqual(q.read32(base+0x34), 0)

    def test_missing_motor_nacks_and_next_transfer_recovers(self):
        q, base = self.q, BASES[4]
        q.write32(base+0x30, ABORT | STOP)
        q.write32(base+0x6c, 1)
        for address in (0x58, 0x6b, 0x58):
            q.write32(base+4, address)
            q.write32(base+0x10, 0x200)
            self.assertFalse(q.irq(53), 'completion must wait for a bus cycle')
            q.command('clock_step 22500')
            self.assertTrue(q.irq(53))
            self.assertEqual(q.read32(base+0x80), 1)
            self.assertEqual(q.read32(base+0x2c), ABORT | STOP)
            q.read32(base+0x54)
            self.assertTrue(q.irq(53), 'STOP remains until acknowledged')
            q.read32(base+0x60)
            self.assertFalse(q.irq(53))
            self.assertEqual(q.read32(base+0x80), 0)
            self.assertEqual(q.read32(base+0x78), 0)

    def test_reset_clears_irq_mask_and_fifo_and_rearms(self):
        q, base = self.q, BASES[4]
        q.write32(base+0x30, ABORT)
        q.write32(base+0x6c, 1)
        q.write32(base+4, 0x58)
        q.write32(base+0x10, 0x300)
        q.command('clock_step 22500')
        self.assertTrue(q.irq(53))
        q.qmp('system_reset')
        q.qmp('query-status')
        for offset in (0x30, 0x34, 0x6c, 0x78, 0x80):
            self.assertEqual(q.read32(base+offset), 0)
        self.assertFalse(q.irq(53))
        q.write32(base+0x10, 0x300)
        self.assertEqual(q.read32(base+0x34), 0, 'disabled commands ignored')
        q.write32(base+0x30, TX_EMPTY)
        q.write32(base+0x6c, 1)
        self.assertTrue(q.irq(53))


@unittest.skipUnless(os.environ.get('MIBAND_BEST1502P_QEMU'), 'native QEMU not set')
class Best1502pI2cSlaveTest(Best1502pI2cTest):
    machine_options = (',display-width=432,display-height=514,panel-id=0x450120'
                       ',touch-chip-id=0x4502,touch-i2c-bus=5'
                       ',touch-address=0x20,touch-irq-pin=76')

    def test_repeated_start_polling_read_threshold_irq_and_recovery(self):
        q, base = self.q, BASES[5]
        q.write32(base+4, 0x20)
        q.write32(base+0x38, 1)
        q.write32(base+0x30, RX_FULL | STOP)
        q.write32(base+0x6c, 1)
        # Zinitix little-endian register 0xcc00 is the configured chip ID.
        for command in (0x00, 0xcc, 0x500, 0x300):
            q.write32(base+0x10, command)
        self.assertEqual(q.read32(base+0x74), 4)
        for i in range(4):
            q.command('clock_step 22500')
            if i == 2:
                self.assertEqual(q.read32(base+0x78), 1)
                self.assertFalse(q.irq(54), 'RX_FULL must respect RX_TL')
        self.assertTrue(q.irq(54))
        self.assertEqual(q.read32(base+0x2c), RX_FULL | STOP)
        q.read32(base+0x60)
        self.assertTrue(q.irq(54))
        self.assertEqual(q.read32(base+0x10), 2)
        self.assertFalse(q.irq(54))
        self.assertEqual(q.read32(base+0x10), 0x45)
        self.assertEqual(q.read32(base+0x74), 0)
        self.assertEqual(q.read32(base+0x78), 0)
        self.assertEqual(q.read32(base+0x80), 0)

    def test_fifo_overflow_underflow_and_disable_cancels_queued_commands(self):
        q, base = self.q, BASES[5]
        q.write32(base+4, 0x20)
        q.write32(base+0x30, 1 | 2 | 8)
        q.write32(base+0x6c, 1)
        for _ in range(17):
            q.write32(base+0x10, 0x100)
        self.assertEqual(q.read32(base+0x74), 16)
        self.assertTrue(q.read32(base+0x2c) & 8)
        q.read32(base+0x4c)
        for _ in range(16):
            q.command('clock_step 22500')
        self.assertEqual(q.read32(base+0x78), 16)
        self.assertTrue(q.read32(base+0x70) & 16)
        q.write32(base+0x10, 0x100)
        q.command('clock_step 22500')
        self.assertTrue(q.read32(base+0x2c) & 2)
        q.read32(base+0x48)
        for _ in range(17):
            q.read32(base+0x10)
        self.assertTrue(q.read32(base+0x2c) & 1)
        q.read32(base+0x44)
        self.assertFalse(q.irq(54))
        q.write32(base+0x10, 0x300)
        q.write32(base+0x6c, 0)
        q.command('clock_step 1000000')
        self.assertEqual(q.read32(base+0x78), 0)
        self.assertEqual(q.read32(base+0x34), 0)


if __name__ == '__main__':
    unittest.main()
