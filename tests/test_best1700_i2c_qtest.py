"""BEST1700 I2C register, interrupt and real QEMU I2CBus transactions.

Supply MIBAND_BEST1700_QEMU as for test_best1700_peripherals_qtest. All
controllers and the optional tmp105 test slave live in fresh test processes.
"""

import os
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


I2C_BASES = (0x40006000, 0x40007000, 0x40075000, 0x40076000, 0x40077000)
I2C2 = I2C_BASES[2]
CON = 0x00
TAR = 0x04
DATA_CMD = 0x10
INTR_STAT = 0x2C
INTR_MASK = 0x30
RAW_INTR = 0x34
RX_TL = 0x38
CLR_INTR = 0x40
CLR_RX_UNDER = 0x44
CLR_RX_OVER = 0x48
CLR_TX_ABRT = 0x54
CLR_STOP = 0x60
ENABLE = 0x6C
STATUS = 0x70
TXFLR = 0x74
RXFLR = 0x78
ABRT_SOURCE = 0x80
ENABLE_STATUS = 0x9C
RX_UNDER = 1 << 0
RX_OVER = 1 << 1
RX_FULL = 1 << 2
TX_EMPTY = 1 << 4
TX_ABRT = 1 << 6
STOP_DET = 1 << 9
CMD_READ = 1 << 8
CMD_STOP = 1 << 9
CMD_RESTART = 1 << 10


class I2cTestCase(unittest.TestCase):
    def start_machine(self, *, slave=False):
        args = ('-device', 'tmp105,id=test-sensor,bus=/best1700-i2c2/i2c,address=0x48') if slave else ()
        self.machine = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'], extra_args=args)
        self.addCleanup(self.machine.close)
        self.machine.command('irq_intercept_in /machine/core[0]')
        return self.machine

    def configure(self, target=0x48, *, base=I2C2, mask=0):
        q = self.machine
        q.write32(base + ENABLE, 0)
        q.write32(base + CON, 0x63)  # Master, standard speed, repeated START.
        q.write32(base + TAR, target)
        q.write32(base + INTR_MASK, mask)
        q.write32(base + ENABLE, 1)


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700I2cQtest(I2cTestCase):
    def setUp(self):
        self.start_machine()

    def test_missing_slave_nacks_reads_and_writes_with_masked_irq52(self):
        q = self.machine
        for command in (0x5A | CMD_STOP, CMD_READ | CMD_STOP):
            with self.subTest(command=hex(command)):
                self.configure()
                q.write32(I2C2 + DATA_CMD, command)
                self.assertEqual(q.read32(I2C2 + ABRT_SOURCE), 1,
                                 'absent seven-bit address must report address NACK')
                self.assertEqual(q.read32(I2C2 + RAW_INTR) & (TX_ABRT | STOP_DET),
                                 TX_ABRT | STOP_DET)
                self.assertEqual(q.read32(I2C2 + INTR_STAT), 0)
                self.assertEqual(q.read32(I2C2 + RXFLR), 0,
                                 'absent slave must not produce fabricated data')
                self.assertEqual(q.read32(I2C2 + STATUS) & 0x21, 0)
                self.assertFalse(q.irq(52))
                q.write32(I2C2 + INTR_MASK, TX_ABRT | STOP_DET)
                self.assertTrue(q.irq(52))
                q.read32(I2C2 + CLR_STOP)
                self.assertEqual(q.read32(I2C2 + INTR_STAT), TX_ABRT)
                self.assertEqual(q.read32(I2C2 + ABRT_SOURCE), 1)
                self.assertTrue(q.irq(52), 'clearing STOP must retain the abort')
                q.read32(I2C2 + CLR_TX_ABRT)
                self.assertEqual(q.read32(I2C2 + ABRT_SOURCE), 0)
                self.assertEqual(q.read32(I2C2 + INTR_STAT), 0)
                self.assertFalse(q.irq(52))
                # Clearing the abort unlocks commands, so the next address
                # probe must make a new attempt and report a fresh failure.
                q.write32(I2C2 + DATA_CMD, command)
                self.assertEqual(q.read32(I2C2 + ABRT_SOURCE), 1)
                self.assertTrue(q.irq(52))
                q.read32(I2C2 + CLR_INTR)
                self.assertEqual(q.read32(I2C2 + ABRT_SOURCE), 0)
                self.assertEqual(q.read32(I2C2 + RAW_INTR) & (TX_ABRT | STOP_DET), 0)
                self.assertFalse(q.irq(52))

    def test_all_five_controllers_route_independent_interrupts(self):
        q = self.machine
        for index, base in enumerate(I2C_BASES):
            with self.subTest(controller=index):
                self.configure(base=base, mask=TX_ABRT)
                q.write32(base + DATA_CMD, CMD_STOP)
                for peer in range(5):
                    self.assertEqual(q.irq(50 + peer), index == peer)
                q.read32(base + CLR_TX_ABRT)
                self.assertFalse(q.irq(50 + index))

    def test_enable_status_disabled_commands_and_system_reset(self):
        q = self.machine
        self.assertEqual(q.read32(I2C2 + ENABLE_STATUS), 0)
        q.write32(I2C2 + DATA_CMD, CMD_READ | CMD_STOP)
        self.assertEqual(q.read32(I2C2 + RAW_INTR), 0)
        self.assertEqual(q.read32(I2C2 + RXFLR), 0)
        self.configure(mask=TX_ABRT)
        self.assertEqual(q.read32(I2C2 + ENABLE_STATUS), 1)
        q.write32(I2C2 + DATA_CMD, CMD_READ | CMD_STOP)
        self.assertTrue(q.irq(52))
        q.qmp('system_reset')
        q.qmp('query-status')
        self.assertEqual(q.read32(I2C2 + ENABLE_STATUS), 0)
        self.assertEqual(q.read32(I2C2 + INTR_MASK), 0)
        self.assertEqual(q.read32(I2C2 + RAW_INTR), 0)
        self.assertEqual(q.read32(I2C2 + ABRT_SOURCE), 0)
        self.assertEqual(q.read32(I2C2 + RXFLR), 0)
        self.assertFalse(q.irq(52))
        self.configure(mask=TX_ABRT)
        q.write32(I2C2 + DATA_CMD, CMD_STOP)
        self.assertTrue(q.irq(52), 'reset must leave the controller usable')
        q.write32(I2C2 + ENABLE, 0)
        self.assertEqual(q.read32(I2C2 + ENABLE_STATUS), 0)
        self.assertFalse(q.irq(52))


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700I2cSlaveQtest(I2cTestCase):
    def setUp(self):
        self.start_machine(slave=True)
        self.configure()

    def write_limit(self, high, low):
        q = self.machine
        for command in (2, high, low | CMD_STOP):
            q.write32(I2C2 + DATA_CMD, command)
        self.assertEqual(q.read32(I2C2 + ABRT_SOURCE), 0)

    def test_tmp105_write_repeated_start_reads_real_bytes_and_rx_threshold(self):
        q = self.machine
        self.write_limit(0x12, 0x34)
        self.assertEqual(q.read32(I2C2 + TXFLR), 0)
        self.assertEqual(q.read32(I2C2 + RAW_INTR) & STOP_DET, STOP_DET)
        q.read32(I2C2 + CLR_STOP)
        q.write32(I2C2 + RX_TL, 1)
        q.write32(I2C2 + INTR_MASK, RX_FULL | STOP_DET)
        q.write32(I2C2 + DATA_CMD, 2)
        self.assertEqual(q.read32(I2C2 + STATUS) & 0x21, 0x21)
        q.write32(I2C2 + DATA_CMD, CMD_READ | CMD_RESTART)
        self.assertEqual(q.read32(I2C2 + RXFLR), 1)
        self.assertEqual(q.read32(I2C2 + RAW_INTR) & STOP_DET, 0,
                         'repeated START must keep the transfer active')
        self.assertFalse(q.irq(52), 'one byte is below RX_TL=1 threshold')
        q.write32(I2C2 + DATA_CMD, CMD_READ | CMD_STOP)
        self.assertEqual(q.read32(I2C2 + RXFLR), 2)
        self.assertEqual(q.read32(I2C2 + INTR_STAT), RX_FULL | STOP_DET)
        self.assertEqual(q.read32(I2C2 + STATUS) & 0x21, 0)
        self.assertTrue(q.irq(52))
        q.read32(I2C2 + CLR_STOP)
        self.assertEqual(q.read32(I2C2 + INTR_STAT), RX_FULL)
        self.assertEqual(q.read32(I2C2 + DATA_CMD), 0x12)
        self.assertEqual(q.read32(I2C2 + RXFLR), 1)
        self.assertFalse(q.irq(52))
        self.assertEqual(q.read32(I2C2 + DATA_CMD), 0x34)
        self.assertEqual(q.read32(I2C2 + RXFLR), 0)
        self.assertEqual(q.read32(I2C2 + STATUS) & (1 << 3), 0)
        self.assertEqual(q.read32(I2C2 + ABRT_SOURCE), 0)

    def test_tmp105_fifo_overflow_underflow_and_disable_flush(self):
        q = self.machine
        self.write_limit(0x42, 0x31)
        q.read32(I2C2 + CLR_INTR)
        q.write32(I2C2 + INTR_MASK, RX_OVER | RX_UNDER)
        q.write32(I2C2 + DATA_CMD, 2)
        for index in range(9):
            q.write32(I2C2 + DATA_CMD, CMD_READ |
                      (CMD_RESTART if index == 0 else 0) |
                      (CMD_STOP if index == 8 else 0))
        self.assertEqual(q.read32(I2C2 + RXFLR), 8)
        self.assertEqual(q.read32(I2C2 + STATUS) & (1 << 4), 1 << 4)
        self.assertEqual(q.read32(I2C2 + INTR_STAT), RX_OVER)
        self.assertTrue(q.irq(52))
        q.read32(I2C2 + CLR_RX_OVER)
        self.assertFalse(q.irq(52))
        self.assertEqual([q.read32(I2C2 + DATA_CMD) for _ in range(8)],
                         [0x42, 0x31] + [0xFF] * 6)
        self.assertEqual(q.read32(I2C2 + RXFLR), 0)
        q.read32(I2C2 + DATA_CMD)
        self.assertEqual(q.read32(I2C2 + INTR_STAT), RX_UNDER)
        self.assertTrue(q.irq(52))
        q.read32(I2C2 + CLR_RX_UNDER)
        self.assertFalse(q.irq(52))
        q.write32(I2C2 + DATA_CMD, 2)
        q.write32(I2C2 + DATA_CMD, CMD_READ | CMD_RESTART)
        self.assertEqual(q.read32(I2C2 + RXFLR), 1)
        q.write32(I2C2 + ENABLE, 0)
        self.assertEqual(q.read32(I2C2 + ENABLE_STATUS), 0)
        self.assertEqual(q.read32(I2C2 + RXFLR), 0)
        self.assertEqual(q.read32(I2C2 + STATUS) & 0x21, 0)
        q.write32(I2C2 + ENABLE, 1)
        q.write32(I2C2 + DATA_CMD, 2)
        q.write32(I2C2 + DATA_CMD, CMD_READ | CMD_RESTART | CMD_STOP)
        self.assertEqual(q.read32(I2C2 + DATA_CMD), 0x42,
                         'disable must release the bus for a new transaction')


if __name__ == '__main__':
    unittest.main()
