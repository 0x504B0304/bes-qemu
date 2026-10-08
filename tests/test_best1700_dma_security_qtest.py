"""Check AP HDMA master attribution with real Secure M55 and MPC traffic.

The guest copies protected destinations into secure DTCM for inspection.
Unspecified qtest memory reads cannot prove delivery to nonsecure PSRAM.
"""
import os
from pathlib import Path
import socket
import struct
import tempfile
import time
import unittest

from tests import test_best1700_mpc_qtest as mpc
from tests.test_best1700_peripherals_qtest import QtestMachine

RESULT = 0x20001000
MASTER = 0x44088048
TARGET = 0x18008000
SOURCE = TARGET + 0x100
LLI = TARGET + 0x200
READY, DONE, FAULT = 0xead1, 0x600d, 0xfa17
PAYLOAD = b'hello!'


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700DmaSecurityQtest(unittest.TestCase):
    assemble = mpc.Best1700MpcQtest.assemble

    def guest_source(self, master_ns, target_ns, *, controller=0, lli=False,
                     uart=True, reset=False):
        dma = 0x400a0000 + controller * 0x8000
        control = 0x88000000 if uart else 0x8c000000
        count = 3 if lli else 6
        setup = f'''
    ldr r5, ={TARGET:#x}
    ldr r0, =0xa5a5a5a5
    str r0, [r5]
    str r0, [r5, #4]
    ldr r0, =0x6c6c6568
    str r0, [r5, #0x100]
    movw r0, #0x216f
    strh r0, [r5, #0x104]
    ldr r6, ={MASTER:#x}
    ldr r0, [r6]
    str r0, [r4, #20]
'''
        if reset:
            setup += '''
    ldr r1, [r4, #0x300]
    cmp r1, #0
    bne master_set
'''
        # Set only this controller's policy bit, leaving the other at reset.
        setup += f'ldr r0, ={(1 << (7 + controller)) if master_ns else 0:#x}\nstr r0, [r6]\nmaster_set:\n'
        if lli:
            setup += f'''
    ldr r6, ={LLI:#x}
    ldr r0, ={0x4000b000 if uart else SOURCE + 3:#x}
    str r0, [r6]
    ldr r0, ={TARGET + 3:#x}
    str r0, [r6, #4]
    movs r0, #0
    str r0, [r6, #8]
    ldr r0, ={control | 3:#x}
    str r0, [r6, #12]
'''
        setup += mpc.lut_word(mpc.PSRAM_MPC, TARGET - 0x18000000,
                              0xffffffff if target_ns else 0)
        setup += mpc.sau_regions([(TARGET, LLI + 31, False)] if target_ns else [])
        if uart:
            setup += '''
    ldr r5, =0x4000b000
    movs r0, #0x70
    str r0, [r5, #0x2c]
    movw r0, #0x301
    str r0, [r5, #0x30]
    movs r0, #1
    str r0, [r5, #0x48]
'''
        setup += f'''
    ldr r5, ={dma:#x}
    movs r0, #1
    str r0, [r5, #0x30]
    addw r5, r5, #0x120
    ldr r0, ={0x4000b000 if uart else SOURCE:#x}
    str r0, [r5]
    ldr r0, ={TARGET:#x}
    str r0, [r5, #4]
    ldr r0, ={LLI if lli else 0:#x}
    str r0, [r5, #8]
    ldr r0, ={control | count:#x}
    str r0, [r5, #12]
    movw r0, #{0xd009 if uart else 0xc001}
    str r0, [r5, #16]
    ldr r0, ={READY:#x}
    str r0, [r4]
wait_host:
    ldr r0, [r4, #4]
    cmp r0, #0
    beq wait_host
    ldr r5, ={mpc.PSRAM_MPC:#x}
    ldr r0, [r5, #0x20]
    str r0, [r4, #8]
    ldr r0, [r5, #0x2c]
    str r0, [r4, #12]
    ldr r0, [r5, #0x30]
    str r0, [r4, #16]
    ldr r5, ={TARGET:#x}
    ldr r0, [r5]
    str r0, [r4, #32]
    ldr r0, [r5, #4]
    str r0, [r4, #36]
    ldr r0, ={DONE:#x}
    str r0, [r4]
halt:
    b halt
'''
        return f'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.word 0x2415f000
.word reset
.rept 142
.word fault
.endr
.org 0x400
.thumb_func
reset:
    cpsid i
    ldr r4, =0x24001000
    movs r0, #0
    movs r1, #0
clear_result:
    str r0, [r4, r1]
    adds r1, #4
    cmp r1, #128
    bne clear_result
    ldr r0, =0x100c0000
    ldr r1, =0xe000ed08
    str r0, [r1]
''' + setup + f'''
.thumb_func
fault:
    ldr r4, =0x24001000
    mrs r0, IPSR
    str r0, [r4, #24]
    ldr r0, ={FAULT:#x}
    str r0, [r4]
    b halt
.ltorg
'''

    def await_value(self, q, reader, message):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            result = reader()
            if result:
                return result
            if q.read32(RESULT) == FAULT:
                self.fail(q.diagnostic('guest exception'))
            time.sleep(.005)
        self.fail(q.diagnostic(message))

    def run_case(self, master_ns, target_ns, *, controller=0, lli=False,
                 uart=True, reset=False):
        with tempfile.TemporaryDirectory(prefix='best1700-hdma-security-') as temp:
            directory = Path(temp)
            serial = directory / 'uart.sock'
            tee = self.assemble(directory, 'tee', self.guest_source(
                master_ns, target_ns, controller=controller, lli=lli,
                uart=uart, reset=reset), 0x100c0000)
            q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                accel='tcg,thread=multi',
                machine_options=(',secure-boot=on,boot-sp=0x2415f000,boot-pc=0x100c0401'
                                 f',tee-image={tee},tee-flash-base=0x10000000'
                                 ',tee-code-offset=0xc0000,tee-entry=0x100c0401'),
                extra_args=('-S', '-chardev',
                            f'socket,id=uart,path={serial},server=on,wait=off'),
                serial_backends=['chardev:uart', 'null', 'null'])
            try:
                self.await_value(q, serial.exists, 'serial did not start')
                with socket.socket(socket.AF_UNIX) as host:
                    host.settimeout(5)
                    host.connect(str(serial))
                    results = []
                    for cycle in range(2 if reset else 1):
                        if cycle:
                            q.qmp('system_reset')
                            q.qmp('query-status')
                            q.write32(RESULT, 0)
                            q.write32(RESULT + 0x300, 1)
                        q.qmp('cont')
                        self.await_value(q, lambda: q.read32(RESULT) == READY,
                                         'guest not ready')
                        if uart:
                            host.sendall(PAYLOAD)
                        channel = 0x401a0000 + controller * 0x8000 + 0x120
                        self.await_value(q, lambda: not q.read32(channel + 16) & 1,
                                         'DMA did not finish')
                        q.write32(RESULT + 4, 1)
                        self.await_value(q, lambda: q.read32(RESULT) == DONE,
                                         'guest did not inspect result')
                        q.qmp('stop')
                        results.append(struct.unpack('<10I', q.read(RESULT, 40)))
                    return results
            finally:
                q.close()

    def test_uart_rx_matching_and_denied_secure_nonsecure_destinations(self):
        for master_ns in (False, True):
            for target_ns in (False, True):
                with self.subTest(master_ns=master_ns, target_ns=target_ns):
                    values, = self.run_case(master_ns, target_ns)
                    data = struct.pack('<2I', *values[8:10])
                    if master_ns == target_ns:
                        self.assertEqual(data, PAYLOAD + b'\xa5\xa5')
                        self.assertEqual(values[2], 0)
                    else:
                        self.assertEqual(data, b'\xa5' * 8)
                        self.assertEqual(values[2] & 1, 1)
                        self.assertEqual(values[3], TARGET - 0x18000000)
                        self.assertEqual(values[4] >> 16 & 1, int(master_ns))

    def test_uart_rx_nonsecure_linked_descriptor_and_destination(self):
        values, = self.run_case(True, True, lli=True)
        self.assertEqual(struct.pack('<2I', *values[8:10]), PAYLOAD + b'\xa5\xa5')
        self.assertEqual(values[2], 0)

    def test_dma1_memory_reads_and_descriptor_use_independent_master_bit(self):
        for nonsecure in (False, True):
            with self.subTest(nonsecure=nonsecure):
                values, = self.run_case(nonsecure, nonsecure, controller=1,
                                        lli=True, uart=False)
                self.assertEqual(struct.pack('<2I', *values[8:10]),
                                 PAYLOAD + b'\xa5\xa5')
                self.assertEqual(values[2], 0)

    def test_machine_reset_revokes_nonsecure_dma_master_ownership(self):
        first, second = self.run_case(True, True, reset=True)
        self.assertEqual(struct.pack('<2I', *first[8:10]), PAYLOAD + b'\xa5\xa5')
        self.assertEqual(struct.pack('<2I', *second[8:10]), b'\xa5' * 8)
        self.assertEqual(first[5], 0)
        self.assertEqual(second[5], 0)
        self.assertEqual(second[2] & 1, 1)


if __name__ == '__main__':
    unittest.main()
