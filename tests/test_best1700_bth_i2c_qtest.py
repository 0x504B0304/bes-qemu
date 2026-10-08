"""Check BTH I2C bus isolation, IRQ routing and real M33 polling.

These are integration checks for the BTH instances. The shared controller's
FIFO and slave-transaction contracts remain covered by the SYS I2C tests.
"""

import os
import struct
import time
import unittest

from tests import test_best1700_bth_qtest as bth
from tests import test_best1700_i2c_qtest as i2c
from tests.test_best1700_peripherals_qtest import QtestMachine


BASES = (0x50006000, 0x50007000, 0x50008000, 0x5000A000, 0x5000D000)


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class BthI2cQtest(i2c.I2cTestCase):
    def start_bth_machine(self, *, tcg=False, intercept=2):
        self.machine = QtestMachine(
            os.environ['MIBAND_BEST1700_QEMU'],
            machine_options=(',bth-real-boot=on,boot-sp=0x203ff000'
                             ',boot-pc=0x101c0001'),
            image_payload=b'\xfe\xe7' + bytes(32),
            accel='tcg' if tcg else 'qtest',
            extra_args=('-S', '-icount', 'shift=3,sleep=off') if tcg else ())
        self.addCleanup(self.machine.close)
        if not tcg:
            self.machine.command(f'irq_intercept_in /machine/core[{intercept}]')
        return self.machine

    def test_empty_bus_nacks_without_fabricated_data_or_busy_poll(self):
        q = self.start_bth_machine()
        base = BASES[1]
        self.configure(base=base)
        q.write32(base + i2c.DATA_CMD, i2c.CMD_READ | i2c.CMD_STOP)
        self.assertEqual(q.read32(base + i2c.ABRT_SOURCE), 1)
        self.assertEqual(q.read32(base + i2c.RXFLR), 0)
        self.assertEqual(q.read32(base + i2c.STATUS), 6,
                         'NACK must leave TX empty, room available and bus idle')
        self.assertEqual(q.read32(base + i2c.RAW_INTR) &
                         (i2c.TX_ABRT | i2c.STOP_DET), i2c.TX_ABRT | i2c.STOP_DET)
        self.assertFalse(q.irq(40))
        q.write32(base + i2c.INTR_MASK, i2c.TX_ABRT)
        self.assertTrue(q.irq(40))
        q.read32(base + i2c.CLR_INTR)
        self.assertFalse(q.irq(40))

    def test_five_irq_lines_and_sys_registers_remain_independent(self):
        q = self.start_bth_machine()
        for index, base in enumerate(BASES):
            with self.subTest(controller=index):
                self.configure(base=base, mask=i2c.TX_ABRT)
                q.write32(base + i2c.DATA_CMD, i2c.CMD_STOP)
                for peer in range(5):
                    self.assertEqual(q.irq(39 + peer), peer == index)
                for sys in i2c.I2C_BASES:
                    self.assertEqual(q.read32(sys + i2c.ENABLE_STATUS), 0)
                    self.assertEqual(q.read32(sys + i2c.ABRT_SOURCE), 0)
                q.read32(base + i2c.CLR_INTR)
                self.assertFalse(q.irq(39 + index))
        for sys in i2c.I2C_BASES:
            self.configure(base=sys, mask=i2c.TX_ABRT)
            q.write32(sys + i2c.DATA_CMD, i2c.CMD_STOP)
            for index, base in enumerate(BASES):
                self.assertFalse(q.irq(39 + index))
                self.assertEqual(q.read32(base + i2c.ABRT_SOURCE), 0)
            q.read32(sys + i2c.CLR_INTR)
        # Observe AP's inputs in a separate process: BTH controller IRQs
        # must not accidentally be connected to AP's same-numbered pins.
        q = self.start_bth_machine(intercept=0)
        for base in BASES:
            self.configure(base=base, mask=i2c.TX_ABRT)
            q.write32(base + i2c.DATA_CMD, i2c.CMD_STOP)
            for irq in (*range(39, 44), *range(50, 55)):
                self.assertFalse(q.irq(irq))

    def test_reset_clears_all_five_controllers_and_interrupts(self):
        q = self.start_bth_machine()
        for base in BASES:
            self.configure(base=base, mask=i2c.TX_ABRT)
            q.write32(base + i2c.DATA_CMD, i2c.CMD_STOP)
        for irq in range(39, 44):
            self.assertTrue(q.irq(irq))
        q.qmp('system_reset')
        q.qmp('query-status')
        for index, base in enumerate(BASES):
            for offset in (i2c.ENABLE_STATUS, i2c.INTR_MASK, i2c.RAW_INTR,
                           i2c.ABRT_SOURCE, i2c.RXFLR):
                self.assertEqual(q.read32(base + offset), 0)
            self.assertFalse(q.irq(39 + index))
            self.configure(base=base, mask=i2c.TX_ABRT)
            q.write32(base + i2c.DATA_CMD, i2c.CMD_STOP)
            self.assertTrue(q.irq(39 + index), 'controller must work after reset')
            q.read32(base + i2c.CLR_INTR)

    def test_real_m33_completes_address_probe_and_status_poll(self):
        code = bth.BthTests.assemble(self, r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x50007000
    ldr r5, =0x23001000
    movs r0, #0x63
    str r0, [r4]
    movs r0, #0x48
    str r0, [r4, #4]
    movs r0, #1
    str r0, [r4, #0x6c]
    mov r0, #0x300
    str r0, [r4, #0x10]
1:  ldr r0, [r4, #0x70]
    tst r0, #0x21
    bne 1b
    str r0, [r5, #4]
    ldr r0, [r4, #0x80]
    str r0, [r5, #8]
    ldr r0, [r4, #0x78]
    str r0, [r5, #12]
    ldr r0, [r4, #0x34]
    str r0, [r5, #16]
    ldr r0, [r4, #0x9c]
    str r0, [r5, #20]
    movs r0, #0xa1
    str r0, [r5]
2:  wfi
    b 2b
.ltorg
''', base=0x03004000)
        q = self.start_bth_machine(tcg=True)
        q.write32(i2c.I2C_BASES[1] + i2c.TAR, 0x55)
        q.write(0x23004000, code)
        q.write(0x23002000, struct.pack('<II', 0x230BFF00, 0x03004001))
        q.write32(bth.VTOR, 0x23002000)
        q.write32(bth.RESET_CLEAR, 0x300)
        q.qmp('cont')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and q.read32(bth.RESULT) != 0xA1:
            time.sleep(0.01)
        q.qmp('stop')
        values = struct.unpack('<6I', q.read(bth.RESULT, 24))
        self.assertEqual(values[:4], (0xA1, 6, 1, 0), q.diagnostic('BTH I2C poll'))
        self.assertEqual(values[4] & (i2c.TX_ABRT | i2c.STOP_DET),
                         i2c.TX_ABRT | i2c.STOP_DET)
        self.assertEqual(values[5], 1)
        self.assertEqual(q.read32(BASES[1] + i2c.ABRT_SOURCE), values[2],
                         'AP and BTH must observe the same controller')
        self.assertEqual(q.read32(i2c.I2C_BASES[1] + i2c.TAR), 0x55)
        self.assertEqual(q.read32(i2c.I2C_BASES[1] + i2c.ENABLE_STATUS), 0)


if __name__ == '__main__':
    unittest.main()
