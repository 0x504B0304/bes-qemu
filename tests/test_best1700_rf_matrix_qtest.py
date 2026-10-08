"""Execute CP packing of RF route bytes into the shared configuration bank."""
import os
import struct
import unittest

from tests import test_best1700_cp_execution_qtest as cp_qtest

BASE = 0x72c0b6c8


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class RfMatrixTests(unittest.TestCase):
    assemble = cp_qtest.CpExecutionQtest.assemble
    machine = cp_qtest.CpExecutionQtest.machine
    await_value = cp_qtest.CpExecutionQtest.await_value

    def test_original_cp_byte_packing_shared_readback_and_reset(self):
        code = self.assemble(r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r0, =table
    ldr r6, =0x72c0b6c8
    movs r3, #0
word:
    movs r2, #0
    movs r1, #0
    adds r5, r0, r3
byte:
    ldrb r4, [r5], #1
    lsls r4, r2
    orrs r1, r4
    adds r2, #8
    cmp r2, #32
    bne byte
    str r1, [r3, r6]
    adds r3, #4
    cmp r3, #32
    bne word
    ldr r0, =0x18008000
    movs r1, #0x42
    str r1, [r0]
1:  wfi
    b 1b
    .ltorg
table:
    .byte 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
    .byte 16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
''', cp_qtest.CODE)
        q = self.machine()
        q.write(cp_qtest.CODE, code)
        cp_qtest.CpExecutionQtest.vector(q)
        q.write32(cp_qtest.RESET_CLEAR, 1 << 6)
        q.write32(cp_qtest.CORE_WAIT, 0)
        q.qmp('cont')
        self.await_value(q, cp_qtest.RESULT, lambda v: v == 0x42,
                         'CP RF route configuration did not finish')
        q.qmp('stop')
        expected = bytes(range(32))
        self.assertEqual(q.read(BASE, 32), expected)
        # Bank retention follows the peripheral domain, not CPU reset.
        q.write32(cp_qtest.RESET_SET, 1 << 6)
        self.assertEqual(q.read(BASE, 32), expected)
        q.command(f'writeb {BASE:#x} 255')
        q.command(f'writew {BASE:#x} 65535')
        q.command(f'writel {BASE + 1:#x} 0xffffffff')
        self.assertEqual(q.read(BASE, 32), expected)
        for addr in (BASE - 4, BASE + 32):
            q.write32(addr, 0xabcdef01)
        self.assertEqual(q.read(BASE, 32), expected)
        q.qmp('system_reset')
        q.qmp('query-status')
        self.assertEqual(q.read(BASE, 32), bytes(32))
        self.assertNotIn('Lockup', q.stderr_path.read_text(errors='replace'))
