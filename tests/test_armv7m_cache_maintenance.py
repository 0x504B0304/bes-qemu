"""Native TCG cache-maintenance NOPs preserve privilege and NVIC state."""
import os
import struct
import tempfile
from pathlib import Path
import time
import unittest
from tests.test_best1700_peripherals_qtest import QtestMachine
from tests import test_best1700_bth_qtest as bth_helpers

RESULT = 0x20001000

@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'), 'QEMU is not set')
class CacheMaintenanceTests(unittest.TestCase):
    assemble = bth_helpers.BthTests.assemble

    def guest(self, body, mode='mttcg', secure=False):
        source = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.word 0x203ff000
.word reset + 1
.word fault + 1
.word fault + 1
.org 0x100
reset:
    ldr r0, =0xe000ed08
    ldr r1, =0x101c0000
    str r1, [r0]
""" + body + r"""
    ldr r0, =0x20001000
    ldr r1, =0xc0decafe
    str r1, [r0]
wait:
    wfi
    b wait
fault:
    ldr r0, =0x20001000
    ldr r1, =0xfa17fa17
    str r1, [r0]
    ldr r2, =0xe000ed28
    ldr r1, [r2]
    str r1, [r0, #8]
    ldr r1, [r2, #4]
    str r1, [r0, #12]
    b wait
"""
        args = ['-S', '-smp', '4']
        args += ['-icount', 'shift=3,sleep=off'] if mode == 'icount' else []
        base = 0x100c0000 if secure else 0x101c0000
        options = (',boot-sp=0x203ff000,boot-pc=0x101c0101'
                   ',cp-real-boot=on,bth-real-boot=on,btc-real-boot=on')
        if secure:
            source = source.replace('0x101c0000', '0x100c0000').replace('0x203ff000', '0x2415f000')
            tmp = tempfile.TemporaryDirectory(prefix='cache-secure-guest-')
            self.addCleanup(tmp.cleanup)
            tee = Path(tmp.name) / 'tee.bin'
            tee.write_bytes(self.assemble(source, base=base))
            options = (',boot-sp=0x2415f000,boot-pc=0x100c0101'
                       ',cp-real-boot=on,bth-real-boot=on,btc-real-boot=on'
                       f',secure-boot=on,tee-image={tee},tee-code-offset=0xc0000'
                       ',tee-flash-base=0x10000000,tee-entry=0x100c0101')
        q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
            image_payload=(b'\xfe\xe7' + bytes(32)) if secure else self.assemble(source, base=base),
            machine_options=options,
            accel='tcg,thread=multi' if mode == 'mttcg' else 'tcg', extra_args=args)
        self.addCleanup(q.close)
        q.read32(RESULT)
        q.qmp('cont')
        end = time.monotonic() + 10
        while time.monotonic() < end:
            result = q.read32(RESULT)
            if result:
                return q, result
            time.sleep(.01)
        self.fail(q.diagnostic('guest completion'))

    def test_privileged_cache_loop_and_stateful_nvic_registers(self):
        for mode in ['mttcg', 'icount']:
            with self.subTest(mode=mode):
                q, result = self.guest(r"""
    ldr r0, =0xe000ef50
    ldr r1, =0x20000000
    ldr r2, =0x1000
loop:
    str r1, [r0]
    str r1, [r0, #8]
    str r1, [r0, #12]
    str r1, [r0, #16]
    str r1, [r0, #20]
    str r1, [r0, #24]
    str r1, [r0, #28]
    str r1, [r0, #32]
    str r1, [r0, #36]
    str r1, [r0, #40]
    adds r1, #16
    subs r2, #1
    bne loop
    dsb
    isb
    ldr r0, =0xe000e200
    movs r1, #1
    str r1, [r0]
    ldr r2, [r0]
    ldr r0, =0x20001004
    str r2, [r0]
""", mode)
                self.assertEqual(result, 0xc0decafe)
                self.assertEqual(q.read32(RESULT + 4) & 1, 1)
                self.assertEqual(q.read32(0xe000ed28), 0)

    def test_unprivileged_cache_write_still_faults(self):
        for mode in ['mttcg', 'icount']:
            with self.subTest(mode=mode):
                q, result = self.guest(r"""
    ldr r0, =0xe000ef5c
    movs r1, #1
    msr control, r1
    isb
    str r1, [r0]
""", mode, secure=True)
                self.assertEqual(result, 0xfa17fa17)
                self.assertTrue(q.read32(RESULT + 8) & (1 << 9))
                self.assertTrue(q.read32(RESULT + 12) & (1 << 30))

if __name__ == '__main__':
    unittest.main()
