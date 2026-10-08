"""Exercise the shared SYS ISPI controller from a real CP guest and AP bus."""

import os
import unittest

from tests import test_best1700_cp_execution_qtest as cp_qtest


ISPI = 0x4000A000
MEMSC = 0x400000C0
RESULT = 0x18009000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class CpIspiTests(unittest.TestCase):
    assemble = cp_qtest.CpExecutionQtest.assemble
    machine = cp_qtest.CpExecutionQtest.machine
    prepare = cp_qtest.CpExecutionQtest.prepare
    vector = staticmethod(cp_qtest.CpExecutionQtest.vector)
    await_value = cp_qtest.CpExecutionQtest.await_value

    def test_cp_and_ap_share_chip_registers_control_fifo_and_sys_semaphore(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x18009000
    ldr r5, =0x400000c0
    ldr r6, =0x4000a000
    ldr r0, [r5]
    str r0, [r4, #4]
    movs r0, #0xa1
    str r0, [r4]
1:  ldr r0, [r4, #0x20]
    cmp r0, #1
    bne 1b
2:  ldr r0, [r5]
    cmp r0, #1
    bne 2b
    str r0, [r4, #8]
    ldr r0, [r6, #4]
    str r0, [r4, #12]
    ldr r0, =(1 << 28) | (0x42 << 18)
    str r0, [r6, #8]
    ldr r0, [r6, #8]
    str r0, [r4, #16]
    ldr r0, =0x387
    str r0, [r6, #4]
    ldr r0, =(0x42 << 18) | (0x2468 << 2)
    str r0, [r6, #8]
    ldr r0, [r6, #8]
    ldr r0, =(1 << 28) | (0x42 << 18)
    str r0, [r6, #8]
    movs r0, #0xa2
    str r0, [r4]
3:  ldr r0, [r4, #0x20]
    cmp r0, #2
    bne 3b
    ldr r0, [r6, #12]
    str r0, [r4, #20]
    movs r0, #1
    str r0, [r5]
    movs r0, #0xa3
    str r0, [r4]
4:  ldr r0, [r4, #0x20]
    cmp r0, #3
    bne 4b
5:  ldr r0, [r5]
    cmp r0, #1
    bne 5b
    ldr r0, [r6, #4]
    str r0, [r4, #24]
    ldr r0, =(1 << 28) | (0x42 << 18)
    str r0, [r6, #8]
    ldr r0, [r6, #8]
    str r0, [r4, #28]
    movs r0, #1
    str r0, [r5]
    movs r0, #0xa4
    str r0, [r4]
6:  wfi
    b 6b
    .ltorg
""", cp_qtest.CODE)
        q = self.machine()
        self.prepare(q, code)
        self.vector(q)
        self.assertEqual(q.read32(MEMSC), 1)
        q.write32(ISPI + 4, 7 << 7)
        q.write32(ISPI + 8, (0x42 << 18) | (0x1357 << 2))
        q.read32(ISPI + 8)
        q.write32(cp_qtest.RESET_CLEAR, 1 << 6)
        q.write32(cp_qtest.CORE_WAIT, 0)
        q.qmp("cont")

        def phase(value):
            self.await_value(q, RESULT, lambda actual: actual == value,
                             f"missing CP ISPI phase {value:#x}")

        phase(0xA1)
        self.assertEqual(q.read32(RESULT + 4), 0,
                         "CP must observe the SYS semaphore held by AP")
        q.write32(MEMSC, 1)
        q.write32(RESULT + 0x20, 1)
        phase(0xA2)
        self.assertEqual(q.read32(RESULT + 8), 1)
        self.assertEqual(q.read32(RESULT + 12), 7 << 7)
        self.assertEqual(q.read32(RESULT + 16), 0x1357 << 2)
        self.assertEqual(q.read32(MEMSC), 0)
        self.assertEqual(q.read32(ISPI + 4), 0x387)
        self.assertEqual(q.read32(ISPI + 12), 6)
        self.assertEqual(q.read32(ISPI + 8), 0x2468 << 2)
        self.assertEqual(q.read32(ISPI + 12), 2)
        q.write32(RESULT + 0x20, 2)
        phase(0xA3)
        self.assertEqual(q.read32(RESULT + 20), 2,
                         "AP's FIFO read must consume CP's pending response")
        self.assertEqual(q.read32(MEMSC), 1)
        self.assertEqual(q.read32(MEMSC), 0)
        q.write32(ISPI + 4, 0x381)
        q.write32(ISPI + 8, (0x42 << 18) | (0x55AA << 2))
        q.read32(ISPI + 8)
        q.write32(MEMSC, 1)
        q.write32(RESULT + 0x20, 3)
        phase(0xA4)
        q.qmp("stop")
        self.assertEqual(q.read32(RESULT + 24), 0x381)
        self.assertEqual(q.read32(RESULT + 28), 0x55AA << 2)
        self.assertEqual(q.read32(MEMSC), 1)
        self.assertEqual(q.read32(ISPI + 12), 2)


if __name__ == "__main__":
    unittest.main()
