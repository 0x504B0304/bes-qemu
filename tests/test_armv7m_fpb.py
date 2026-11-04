"""ARMv7M FPBv2 execution comparators and DEMCR storage."""
import os
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine

FPB_CTRL = 0xe0002000
FPB_REMAP = 0xe0002004
FPB_COMP0 = 0xe0002008
DEMCR = 0xe000edfc

FPB_SOURCE = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.word 0x2015f000
.word reset + 1
.rept 10
.word fault + 1
.endr
.word debugmon + 1
.word fault + 1
.word fault + 1
.word fault + 1
.org 0x100
.thumb_func
reset:
    ldr r0, =0xe000ed08
    ldr r1, =0x0c0c0000
    str r1, [r0]
    ldr r0, =0xe002ed08
    str r1, [r0]
    ldr r0, =0xe000edfc
    ldr r1, =0x00110000
    str r1, [r0]
    ldr r0, =0xe0002000
    movs r1, #3
    str r1, [r0]
    ldr r0, =0xe0002008
    ldr r1, =target
    orr r1, r1, #1
    str r1, [r0]
    nop
target:
    nop
    ldr r0, =0x20001000
    ldr r1, =0x0bad
    str r1, [r0]
wait:
    b wait
.thumb_func
debugmon:
    ldr r0, =0x20001004
    ldr r1, =0xd00d
    str r1, [r0]
    bx lr
.thumb_func
fault:
    ldr r0, =0x20001008
    ldr r1, =0xfa17
    str r1, [r0]
    b wait
"""


@unittest.skipUnless(os.environ.get('MIBAND_FPB_QEMU'), 'QEMU with FPBv2 is not set')
class Armv7mFpbTests(unittest.TestCase):
    def test_halfword_lanes_key_and_reset(self):
        q = QtestMachine(os.environ['MIBAND_FPB_QEMU'],
                         machine_name='miband-best1502p')
        self.addCleanup(q.close)
        self.assertEqual(int(q.command(f'readw {FPB_CTRL:#x}'), 0), 0x80)
        self.assertEqual(int(q.command(f'readw {FPB_CTRL + 2:#x}'), 0), 0x1000)
        q.command(f'writew {FPB_CTRL:#x} 3')
        self.assertEqual(q.read32(FPB_CTRL), 0x10000081)
        q.command(f'writew {FPB_CTRL + 2:#x} 0')
        self.assertEqual(q.read32(FPB_CTRL), 0x10000081)
        q.command(f'writew {FPB_REMAP:#x} 0x2040')
        q.command(f'writew {FPB_REMAP + 2:#x} 0x2000')
        self.assertEqual(q.read32(FPB_REMAP), 0x20002040)
        for address in (FPB_CTRL + 0xfb0, FPB_CTRL + 0xfb2,
                        FPB_CTRL + 0xfb4, FPB_CTRL + 0xfb6):
            self.assertEqual(int(q.command(f'readw {address:#x}'), 0), 0)
        q.qmp('system_reset')
        self.assertEqual(q.read32(FPB_CTRL), 0x10000080)
        self.assertEqual(q.read32(FPB_COMP0), 0)

    def test_control_comparator_and_demcr(self):
        q = QtestMachine(os.environ['MIBAND_FPB_QEMU'],
                         machine_name='miband-best1502p')
        self.addCleanup(q.close)
        self.assertEqual(q.read32(FPB_CTRL), 0x10000080)
        q.write32(FPB_CTRL, 1)
        self.assertEqual(q.read32(FPB_CTRL), 0x10000080)
        q.write32(FPB_CTRL, 3)
        self.assertEqual(q.read32(FPB_CTRL), 0x10000081)
        q.write32(FPB_REMAP, 0x20002003)
        self.assertEqual(q.read32(FPB_REMAP), 0x20002000)
        q.write32(FPB_COMP0, 0x0c0c0101)
        self.assertEqual(q.read32(FPB_COMP0), 0x0c0c0101)
        q.write32(DEMCR, 0x00010000)
        self.assertEqual(q.read32(DEMCR), 0x00010000)
        q.write32(FPB_CTRL, 2)
        self.assertEqual(q.read32(FPB_CTRL), 0x10000080)

    def test_fpb_match_enters_debugmonitor(self):
        from tests.test_best1700_bth_qtest import BthTests
        helper = BthTests(methodName='test_default_mode_keeps_legacy_independent_ram')
        image = helper.assemble(FPB_SOURCE, base=0x0c0c0000)
        q = QtestMachine(os.environ['MIBAND_FPB_QEMU'], image_payload=image,
                         machine_options=',boot-sp=0x2015f000,boot-pc=0x0c0c0101',
                         accel='tcg', extra_args=['-S'],
                         machine_name='miband-best1502p')
        self.addCleanup(q.close)
        q.qmp('cont')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if q.read32(0x20001004):
                break
            time.sleep(.01)
        self.assertEqual(q.read32(0x20001004), 0xd00d)
        self.assertEqual(q.read32(0x20001008), 0)


if __name__ == '__main__':
    unittest.main()
