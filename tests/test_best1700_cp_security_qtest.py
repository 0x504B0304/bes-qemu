"""Exercise CP bus security with real Secure M55 execution and shared MPCs.

The AP configures the fabric from Secure state. Synthetic peer programs
prove that bus attribution and CPU exception security are independent, and
that changing either policy invalidates already-used memory translations.
"""

import os
from pathlib import Path
import struct
import tempfile
import time
import unittest

from tests import test_best1700_cp_execution_qtest as cp_execution
from tests.test_best1700_mpc_qtest import lut_word
from tests.test_best1700_peripherals_qtest import QtestMachine


PSRAM = 0x1B300000
PSRAM_MPC = 0x44056000
RESULT = 0x20001000
CP_RESULT_AXI = 0x21C01000
SEED = 0x13579BDF
DONE = 0x600D


def policy(enable, nonsecure):
    return f"""
    ldr r5, =0x40088000
    ldr r0, ={(1 << 21) if nonsecure else 0:#x}
    str r0, [r5, #0x48]
    movs r0, #{(1 << 5) if enable else 0}
    str r0, [r5, #0x90]
    dsb sy
    isb sy
"""


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class CpSecurityQtest(unittest.TestCase):
    assemble = cp_execution.CpExecutionQtest.assemble

    def run_guest(self, cp_source, body, *, cp_base=0x00100400):
        cp = self.assemble(cp_source, cp_base)
        cp_bus = 0x20F00400 if cp_base == 0x00100400 else cp_base
        cp_entry = 0x3B300001 if cp_base == PSRAM else cp_base + 1
        vectors = [0x200DFFA0, cp_entry] + [cp_base + 0x301] * 14
        vectors[11] = cp_base + 0x101
        payload = cp + bytes((-len(cp)) % 4) + struct.pack('<16I', *vectors)
        vector_offset = (len(cp) + 3) & ~3
        embedded = '\n'.join('.byte ' + ','.join(str(b) for b in payload[i:i + 16])
                             for i in range(0, len(payload), 16))
        # AP code and stack are Secure. It seeds the peer before changing
        # the MPC LUT, and uses the CP AXI DTCM view for coordination.
        ap = r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.word 0x2415f000
.word reset_a
.rept 142
.word fault
.endr
.org 0x400
.thumb_func
reset_a:
    ldr r4, =0x24001000
    ldr r0, =0x100c0000
    ldr r1, =0xe000ed08
    str r0, [r1]
    ldr r0, =cp_image
    ldr r1, =''' + str(cp_bus) + r'''
    ldr r2, =''' + str(len(cp)) + r'''
1:  ldrb r3, [r0], #1
    strb r3, [r1], #1
    subs r2, #1
    bne 1b
    ldr r0, =cp_image + ''' + str(vector_offset) + r'''
    ldr r1, =0x21cdff00
    movs r2, #16
2:  ldr r3, [r0], #4
    str r3, [r1], #4
    subs r2, #1
    bne 2b
''' + body + r'''
    ldr r0, =0x600d
    str r0, [r4]
3:  wfi
    b 3b
.thumb_func
fault:
    ldr r4, =0x24001000
    mrs r0, ipsr
    str r0, [r4, #4]
    ldr r0, =0xfa17
    str r0, [r4]
4:  wfi
    b 4b
.ltorg
.balign 4
cp_image:
''' + embedded + '\n'
        with tempfile.TemporaryDirectory(prefix='cp-security-contract-') as temp:
            tee = Path(temp) / 'tee.bin'
            tee.write_bytes(self.assemble(ap, 0x100C0000))
            q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                image_payload=b'\xfe\xe7' + bytes(32), accel='tcg',
                extra_args=('-S', '-icount', 'shift=3,sleep=off'),
                machine_options=(',cp-real-boot=on,secure-boot=on'
                    ',boot-sp=0x2415f000,boot-pc=0x100c0401'
                    f',tee-image={tee},tee-flash-base=0x10000000'
                    ',tee-code-offset=0xc0000,tee-entry=0x100c0401'))
            try:
                q.qmp('cont')
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline and q.read32(RESULT) == 0:
                    time.sleep(0.01)
                q.qmp('stop')
                values = struct.unpack('<128I', q.read(RESULT, 512))
                self.assertEqual(values[0], DONE,
                                 f'{values}\n{q.diagnostic("CP security result")}')
                self.assertNotIn('Lockup', q.stderr_path.read_text(errors='replace'))
                return values
            except (RuntimeError, OSError) as error:
                self.fail(f'{error}\n{q.diagnostic("CP security execution")}')
            finally:
                q.close()

    @staticmethod
    def release():
        return r'''
    ldr r5, =0x40080000
    ldr r0, =0x200dff00
    str r0, [r5, #0xb4]
    movs r0, #2
    str r0, [r5, #0x1a4]
    movs r0, #0x40
    str r0, [r5, #0xcc]
    dsb sy
    movs r0, #0
    str r0, [r5, #0x1a4]
'''

    def test_secure_cp_executes_nonsecure_psram_and_keeps_secure_svc_return(self):
        cp = r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r0, =0x200dcf00
    movs r1, #0
    movs r2, #8
1:  str r1, [r0], #4
    subs r2, #1
    bne 1b
    ldr r0, =0x200dcf00
    ldr r1, =0x13579bdf
    str r1, [r0]
    ldr r1, =new_thread
    bic r1, r1, #1
    str r1, [r0, #24]
    mov r1, #0x01000000
    str r1, [r0, #28]
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
    ldr lr, =0xfffffffd
    bx lr
.ltorg
.org 0x200
.thumb_func
new_thread:
    ldr r4, =0x20001000
    str r0, [r4, #4]
    mrs r0, ipsr
    str r0, [r4, #8]
    mrs r0, control
    str r0, [r4, #12]
    ldr r0, =0xe000ede4
    ldr r0, [r0]
    str r0, [r4, #16]
    movs r0, #0x55
    str r0, [r4]
2:  wfi
    b 2b
.ltorg
.org 0x300
.thumb_func
fault:
    ldr r4, =0x20001000
    mrs r0, ipsr
    str r0, [r4, #4]
    movs r0, #0xee
    str r0, [r4]
3:  wfi
    b 3b
.ltorg
'''
        body = lut_word(PSRAM_MPC, PSRAM - 0x18000000, 0xFFFFFFFF)
        body += policy(True, True) + self.release() + r'''
    ldr r5, =0x21c01000
5:  ldr r0, [r5]
    cmp r0, #0
    beq 5b
    str r0, [r4, #4]
    ldr r0, [r5, #4]
    str r0, [r4, #8]
    ldr r0, [r5, #8]
    str r0, [r4, #12]
    ldr r0, [r5, #12]
    str r0, [r4, #16]
    ldr r0, [r5, #16]
    str r0, [r4, #20]
'''
        values = self.run_guest(cp, body, cp_base=PSRAM)
        self.assertEqual(values[1:4], (0x55, SEED, 0))
        self.assertTrue(values[4] & 2, 'new thread must use PSP')
        self.assertEqual(values[5], 0, 'Secure SVC must not create a SecureFault')

    def test_policy_and_mpc_changes_revoke_cached_translations_in_both_aliases(self):
        cp = r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x20001000
    ldr r5, =0x1b300000
    ldr r7, =0x3b300000
    movs r6, #0
1:  ldr r0, [r4, #4]
    cmp r0, r6
    beq 1b
    mov r6, r0
    ldr r0, [r5]
    str r0, [r4, #8]
    ldr r0, [r7]
    str r0, [r4, #16]
    dmb sy
    str r6, [r4, #12]
    b 1b
.ltorg
.org 0x300
.thumb_func
fault:
    b fault
'''
        stages = [(False, False, False), (False, False, True),
                  (True, True, True), (True, False, True),
                  (True, True, True), (True, True, False),
                  (False, True, False)]
        body = f'ldr r5, ={PSRAM:#x}\nldr r0, ={SEED:#x}\nstr r0, [r5]\n'
        body += self.release()
        previous_block_ns = False
        for index, (enabled, ns, block_ns) in enumerate(stages):
            if block_ns != previous_block_ns:
                body += lut_word(PSRAM_MPC, PSRAM - 0x18000000,
                                 0xFFFFFFFF if block_ns else 0)
                previous_block_ns = block_ns
            body += policy(enabled, ns)
            offset = 4 + index * 16
            body += f'''
    ldr r5, =0x44056000
    movs r0, #1
    str r0, [r5, #0x24]
    ldr r6, =0x21c01000
    movs r1, #{index + 1}
    str r1, [r6, #4]
8:  ldr r0, [r6, #12]
    cmp r0, r1
    bne 8b
    ldr r0, [r6, #8]
    str r0, [r4, #{offset}]
    ldr r0, [r6, #16]
    str r0, [r4, #{offset + 4}]
    ldr r0, [r5, #0x20]
    str r0, [r4, #{offset + 8}]
    ldr r0, [r5, #0x30]
    str r0, [r4, #{offset + 12}]
'''
        values = self.run_guest(cp, body)
        for index, (enabled, ns, block_ns) in enumerate(stages):
            with self.subTest(override=enabled, bus_ns=ns, block_ns=block_ns):
                actual_ns = enabled and ns
                allowed = actual_ns == block_ns
                start = 1 + index * 4
                cached, uncached, irq, info = values[start:start + 4]
                self.assertEqual((cached, uncached), (SEED, SEED) if allowed else (0, 0))
                self.assertEqual(irq & 1, 0 if allowed else 1)
                if not allowed:
                    self.assertEqual(info & 0x30000,
                                     (int(actual_ns) << 16) | (int(block_ns) << 17))

    def test_nonsecure_bus_reaches_each_shared_sram_bank_and_alias(self):
        # The peer executes from its own TCM while only the fabric output is
        # forced NS. Each bank has independent MPC state and two bus aliases.
        banks = [(0x02000000 + i * 0x100000,
                  0x22000000 + i * 0x100000,
                  0x4405A000 + i * 0x2000) for i in range(8)]
        reads = ''
        body = ''
        for index, (code, data, mpc) in enumerate(banks):
            body += (f'ldr r5, ={code:#x}\nldr r0, ={SEED + index:#x}\n'
                     'str r0, [r5]\n')
            body += lut_word(mpc, 0, 0xFFFFFFFF)
            reads += f'''
    ldr r5, ={code:#x}
    ldr r0, [r5]
    str r0, [r4, #{4 + index * 8}]
    ldr r5, ={data:#x}
    ldr r0, [r5]
    str r0, [r4, #{8 + index * 8}]
    eor r0, r0, #1
    str r0, [r5]
'''
        cp = r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x20001000
''' + reads + r'''
    dmb sy
    movs r0, #0x55
    str r0, [r4]
1:  wfi
    b 1b
.ltorg
.org 0x300
.thumb_func
fault:
    b fault
'''
        body += policy(True, True) + self.release() + r'''
    ldr r6, =0x21c01000
5:  ldr r0, [r6]
    cmp r0, #0x55
    bne 5b
    add r6, #4
    add r7, r4, #4
    movs r2, #16
6:  ldr r0, [r6], #4
    str r0, [r7], #4
    subs r2, #1
    bne 6b
'''
        for index, (code, data, mpc) in enumerate(banks):
            body += lut_word(mpc, 0, 0)
            body += (f'ldr r5, ={code:#x}\nldr r0, [r5]\n'
                     f'str r0, [r4, #{68 + index * 4}]\n')
        values = self.run_guest(cp, body)
        for index, (code, data, _) in enumerate(banks):
            with self.subTest(code_alias=hex(code), data_alias=hex(data)):
                self.assertEqual(values[1 + index * 2:3 + index * 2],
                                 (SEED + index, SEED + index))
                self.assertEqual(values[17 + index], (SEED + index) ^ 1)


if __name__ == '__main__':
    unittest.main()
