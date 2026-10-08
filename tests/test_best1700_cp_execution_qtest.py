"""Execute synthetic M55C1 programs through guest reset and CORE_WAIT controls.

The programs are assembled in temporary directories. Each test owns its QEMU
process and uses shared guest memory to observe execution; no watch image or
running emulator is required.
"""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


AON = 0x40080000
VTOR = AON + 0xb4
PULSE = AON + 0xc4
RESET_SET = AON + 0xc8
RESET_CLEAR = AON + 0xcc
CORE_WAIT = AON + 0x1a4
CODE = 0x1b300000
CODE_NC = 0x3b300000
RESULT = 0x18008000
VECTOR_A, VECTOR_B = 0x200dff00, 0x200dfd00
STACK_A, STACK_B = 0x200dffa0, 0x200defa0
DTCM_AXI = 0x21c00000
DTCM_SEED = 0xc0ffee42


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class CpExecutionQtest(unittest.TestCase):
    def assemble(self, source, base):
        commands = [shutil.which(name) for name in
                    ('arm-none-eabi-as', 'arm-none-eabi-ld', 'arm-none-eabi-objcopy')]
        if not all(commands):
            self.skipTest('GNU Arm binutils are not installed')
        with tempfile.TemporaryDirectory(prefix='best1700-cp-program-') as temp:
            directory = Path(temp)
            asm, obj = directory / 'test.s', directory / 'test.o'
            elf, binary = directory / 'test.elf', directory / 'test.bin'
            asm.write_text(source)
            for command in (
                [commands[0], '-mthumb', '-o', str(obj), str(asm)],
                [commands[1], f'-Ttext={base:#x}', '-e', 'reset_a',
                 '-o', str(elf), str(obj)],
                [commands[2], '-O', 'binary', str(elf), str(binary)],
            ):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            return binary.read_bytes()

    def cp_program(self):
        return self.assemble(r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    movs r7, #0xa1
    b common
.org 0x100
.thumb_func
reset_b:
    movs r7, #0xb2
common:
    ldr r4, =0x18008000
    ldr r0, [r4, #4]
    adds r0, #1
    str r0, [r4, #4]
    mrs r0, msp
    str r0, [r4, #8]
    ldr r0, =0xe000ed08
    ldr r0, [r0]
    str r0, [r4, #12]
    ldr r0, =0x20000440
    ldr r0, [r0]
    str r0, [r4, #20]
    ldr r0, =0x00100401
    blx r0
    str r0, [r4, #24]
    movs r5, #0
    str r7, [r4]
1:  adds r5, #1
    str r5, [r4, #16]
    b 1b
    .ltorg
''', CODE)

    def machine(self, ap=None):
        machine = QtestMachine(
            os.environ['MIBAND_BEST1700_QEMU'],
            image_payload=ap or (b'\xfe\xe7' + bytes(32)),
            machine_options=',cp-real-boot=on,boot-sp=0x203ff000,boot-pc=0x101c0001',
            accel='tcg', extra_args=('-S', '-icount', 'shift=3,sleep=off'))
        self.addCleanup(machine.close)
        return machine

    @staticmethod
    def core_state(q):
        return json.loads(q.qmp('qom-get', {
            'path': '/machine', 'property': 'core-status'}))['cp']

    def await_value(self, q, address, predicate, message):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            value = q.read32(address)
            if predicate(value):
                return value
            time.sleep(0.01)
        self.fail(q.diagnostic(message))

    def await_state(self, q, expected):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.core_state(q) == expected:
                return
            time.sleep(0.01)
        self.fail(q.diagnostic(f'CP did not enter {expected}'))

    def prepare(self, q, code):
        q.write(CODE, code)
        q.write32(0x20000440, 0x11223344)  # AP-local DTCM must stay independent.
        q.write32(DTCM_AXI + 0x440, DTCM_SEED)
        # movs r0, #0x5a; bx lr. Written through AXI, executed in CP ITCM.
        q.write(0x20f00400, b'\x5a\x20\x70\x47')

    @staticmethod
    def vector(q, address=VECTOR_A, stack=STACK_A, pc=CODE_NC + 1):
        q.write(DTCM_AXI + address - 0x20000000, struct.pack('<II', stack, pc))
        q.write32(VTOR, address)

    def assert_boot(self, q, *, marker=0xa1, stack=STACK_A, vector=VECTOR_A, entries=1):
        self.await_value(q, RESULT, lambda value: value == marker, 'CP entry marker')
        q.qmp('stop')
        values = struct.unpack('<7I', q.read(RESULT, 28))
        self.assertEqual(values[:4], (marker, entries, stack, vector))
        self.assertEqual(values[5:], (DTCM_SEED, 0x5a))
        self.assertEqual(q.read32(0x20000440), 0x11223344)

    def start(self, q):
        self.vector(q)
        q.write32(RESET_CLEAR, 1 << 6)
        q.write32(CORE_WAIT, 0)
        q.qmp('cont')
        self.assert_boot(q)

    def test_reset_release_waits_for_late_guest_vector_pair(self):
        code = self.cp_program()
        q = self.machine()
        self.prepare(q, code)
        q.write32(VTOR, VECTOR_A)
        # Reset is released before the AP has written a usable vector pair.
        q.write(DTCM_AXI + VECTOR_A - 0x20000000, bytes(8))
        q.write32(RESET_CLEAR, 1 << 6)
        q.qmp('cont')
        self.await_state(q, 'waiting')
        time.sleep(0.02)
        self.assertEqual(q.read32(RESULT), 0)
        self.assertEqual(q.qmp('query-status')['status'], 'running')
        self.vector(q)
        q.write32(CORE_WAIT, 0)
        self.assert_boot(q)

    def test_wait_clear_alone_cannot_release_an_asserted_reset(self):
        code = self.cp_program()
        q = self.machine()
        self.prepare(q, code)
        self.vector(q)
        q.write32(CORE_WAIT, 0)
        q.qmp('cont')
        self.await_state(q, 'held-reset')
        time.sleep(0.02)
        self.assertEqual(q.read32(RESULT), 0)
        q.write32(RESET_CLEAR, 1 << 6)
        self.assert_boot(q)

    def test_core_wait_preserves_register_counter_stack_and_current_vector(self):
        code = self.cp_program()
        q = self.machine()
        self.prepare(q, code)
        self.start(q)
        q.qmp('cont')
        self.await_value(q, RESULT + 16, lambda value: value >= 100, 'CP loop counter')
        q.write32(CORE_WAIT, 2)
        self.await_state(q, 'waiting')
        paused = q.read32(RESULT + 16)
        time.sleep(0.03)
        self.assertEqual(q.read32(RESULT + 16), paused)
        self.assertEqual(q.qmp('query-status')['status'], 'running')
        # A changed vector must apply only after an actual reset, never wait.
        self.vector(q, VECTOR_B, STACK_B, CODE_NC + 0x101)
        q.write32(CORE_WAIT, 0)
        self.await_value(q, RESULT + 16, lambda value: value > paused, 'CP register counter resume')
        self.assert_boot(q)

    def test_reset_pulse_reloads_guest_vector_and_restarts_program(self):
        code = self.cp_program()
        q = self.machine()
        self.prepare(q, code)
        self.start(q)
        self.vector(q, VECTOR_B, STACK_B, CODE_NC + 0x101)
        q.write32(PULSE, 1 << 6)
        q.qmp('cont')
        self.assert_boot(q, marker=0xb2, stack=STACK_B, vector=VECTOR_B, entries=2)
        self.assertEqual(q.read32(RESET_CLEAR) & (1 << 6), 1 << 6)
        self.assertEqual(q.read32(CORE_WAIT) & 2, 0)

    def test_ap_guest_populates_psram_tcm_and_vector_before_releasing_cp(self):
        cp = self.cp_program()
        embedded = '\n'.join('.byte ' + ','.join(str(value) for value in cp[offset:offset + 16])
                             for offset in range(0, len(cp), 16))
        ap = self.assemble(r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r0, =0x3b300000
    ldr r1, =cp_image
    ldr r2, =cp_image_end
1:  ldrb r3, [r1], #1
    strb r3, [r0], #1
    cmp r1, r2
    blo 1b
    ldr r0, =0x20000440
    ldr r1, =0x11223344
    str r1, [r0]
    ldr r0, =0x21c00440
    ldr r1, =0xc0ffee42
    str r1, [r0]
    ldr r0, =0x20f00400
    ldr r1, =0x4770205a
    str r1, [r0]
    ldr r5, =0x40080000
    ldr r0, =0x200dff00
    str r0, [r5, #0xb4]
    movs r0, #2
    str r0, [r5, #0x1a4]
    movs r0, #0x40
    str r0, [r5, #0xcc]
    ldr r4, =0x21cdff00
    ldr r0, =0x200dffa0
    str r0, [r4]
    ldr r0, =0x3b300001
    str r0, [r4, #4]
    dsb sy
    movs r0, #0
    str r0, [r5, #0x1a4]
    ldr r4, =0x18008000
2:  ldr r0, [r4]
    cmp r0, #0xa1
    bne 2b
    movs r0, #0xac
    str r0, [r4, #28]
3:  wfi
    b 3b
    .ltorg
    .balign 4
cp_image:
''' + embedded + '\ncp_image_end:\n', 0x101c0000)
        q = self.machine(ap)
        q.qmp('cont')
        self.await_value(q, RESULT + 28, lambda value: value == 0xac,
                          'AP must resume and observe the CP-written shared-memory marker')
        self.assert_boot(q)
        self.assertEqual(q.read(CODE, len(cp)), cp)

    def test_secure_svc_can_start_psp_thread_with_basic_or_fp_exception_frame(self):
        frame_address = 0x200dcf00
        fp_value = 0x3f800000
        for extended, excret in ((False, 0xfffffffd), (True, 0xffffffed)):
            with self.subTest(extended_fp_frame=extended):
                fp_probe = 'vmov r0, s0' if extended else 'movs r0, #0'
                code = self.assemble(r'''
.syntax unified
.arch armv8-m.main
.fpu fpv5-sp-d16
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r0, =0xe000ed88
    ldr r1, =0x00f00000
    str r1, [r0]
    dsb sy
    isb sy
    ldr r0, =0xe000ef34
    movs r1, #0
    str r1, [r0]
    svc #0
    b fault
    .ltorg
.org 0x100
.thumb_func
svc_handler:
    ldr r0, =0x200dcf00
    msr psp, r0
    movs r0, #0
    msr psplim, r0
    ldr lr, =''' + str(excret) + r'''
    bx lr
    .ltorg
.org 0x200
.thumb_func
new_thread:
    ldr r4, =0x18008000
    str r0, [r4, #4]
    mrs r0, ipsr
    str r0, [r4, #8]
    mrs r0, psp
    str r0, [r4, #12]
    mrs r0, control
    str r0, [r4, #16]
    ''' + fp_probe + r'''
    str r0, [r4, #20]
    ldr r0, =0xe000ede4
    ldr r0, [r0]
    str r0, [r4, #24]
    movs r0, #0x55
    str r0, [r4]
1:  wfi
    b 1b
    .ltorg
.org 0x300
.thumb_func
fault:
    ldr r4, =0x18008000
    mrs r0, ipsr
    str r0, [r4, #8]
    ldr r0, =0xe000ed28
    ldr r0, [r0]
    str r0, [r4, #12]
    ldr r0, =0xe000ede4
    ldr r0, [r0]
    str r0, [r4, #24]
    movs r0, #0xee
    str r0, [r4]
2:  wfi
    b 2b
    .ltorg
''', CODE)
                q = self.machine()
                q.write(CODE, code)
                vectors = [STACK_A, CODE_NC + 1] + [CODE_NC + 0x301] * 14
                vectors[11] = CODE_NC + 0x101
                q.write(DTCM_AXI + VECTOR_A - 0x20000000, struct.pack('<16I', *vectors))
                # Architectural exception frame: R0-R3, R12, LR, PC, xPSR,
                # followed by S0-S15/FPSCR/reserved for an extended frame.
                frame = [0x13579bdf, 0, 0, 0, 0, CODE_NC + 0x301,
                         CODE_NC + 0x200, 0x01000000]
                if extended:
                    frame += [fp_value] + [0] * 15 + [0, 0]
                q.write(DTCM_AXI + frame_address - 0x20000000,
                        struct.pack('<' + 'I' * len(frame), *frame))
                q.write32(VTOR, VECTOR_A)
                q.write32(RESET_CLEAR, 1 << 6)
                q.write32(CORE_WAIT, 0)
                q.qmp('cont')
                self.await_value(q, RESULT, lambda value: value in (0x55, 0xee),
                                  'SVC did not return to the synthetic PSP thread')
                q.qmp('stop')
                values = struct.unpack('<7I', q.read(RESULT, 28))
                self.assertEqual(values[0], 0x55,
                                 f'SVC exception return faulted: {[hex(v) for v in values]}')
                self.assertEqual(values[1:4], (0x13579bdf, 0,
                                                frame_address + len(frame) * 4))
                self.assertEqual(values[4] & 6, 6 if extended else 2)
                self.assertEqual(values[5], fp_value if extended else 0)
                self.assertEqual(values[6], 0, 'SVC return must not set SecureFault status')


if __name__ == '__main__':
    unittest.main()
