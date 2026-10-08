"""AMP peers must not execute until the AP releases their hardware controls."""
import os
import struct
import time
import unittest
from tests.test_best1700_peripherals_qtest import QtestMachine
from tests import test_best1700_bth_qtest as bth_tests


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'), 'QEMU required')
class AmpMttcgTests(unittest.TestCase):
    def test_ap_releases_peers_in_sequence_and_cp_wait_keeps_vector_unexecuted(self):
        code = bth_tests.BthTests.assemble(self, r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset
.thumb_func
reset:
    ldr r4, =0x20300000
    ldr r5, =0x40080000
1:  ldr r0, [r4]
    cmp r0, #1
    bne 1b
    movs r0, #64
    str r0, [r5, #0xcc]
    movs r0, #1
    str r0, [r4, #4]
2:  ldr r0, [r4]
    cmp r0, #2
    bne 2b
    ldr r0, =0x21cdff00
    ldr r1, =0x200dfe00
    str r1, [r0]
    ldr r1, =0x200dff09
    str r1, [r0, #4]
    ldr r0, =0x200dff00
    str r0, [r5, #0xb4]
    dsb
    movs r0, #0
    str r0, [r5, #0x1a4]
3:  ldr r0, [r4]
    cmp r0, #3
    bne 3b
    ldr r0, =0x23000300
    str r0, [r5, #0xb8]
    ldr r0, =0x1300
    str r0, [r5, #0xcc]
4:  ldr r0, [r4]
    cmp r0, #4
    bne 4b
    ldr r0, =0x68000400
    str r0, [r5, #0x168]
    movs r0, #0x88
    str r0, [r5, #0xcc]
5:  wfi
    b 5b
.ltorg
''', base=0x101c0000)
        q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'], image_payload=code,
            machine_options=',cp-real-boot=on,bth-real-boot=on,btc-real-boot=on,'
                            'boot-sp=0x203ff000,boot-pc=0x101c0001',
            accel='tcg,thread=multi', extra_args=['-smp', '4', '-S'])
        self.addCleanup(q.close)
        # Each peer writes one marker and enters its own WFI loop.
        def peer(vector, stack, marker, host_vector=None, host_marker=None):
            payload = struct.pack('<II', stack, vector + 9)
            payload += bytes.fromhex('02480121016030bffde700bf') + struct.pack('<I', marker)
            q.write(host_vector or vector, payload)
            q.write32(host_marker or marker, 0)
        peer(0x200dff00, 0x200dfe00, 0x200dfe80, 0x21cdff00, 0x21cdfe80)
        q.write(0x21cdff00, bytes(8))  # AP has not yet populated CP's vectors.
        peer(0x23000300, 0x2315efc0, 0x23001000)
        peer(0x68000400, 0x68002000, 0x68001000)
        markers = [0x21cdfe80, 0x23001000, 0x68001000]
        q.qmp('cont')
        time.sleep(.03)
        self.assertEqual([q.read32(a) for a in markers], [0, 0, 0])
        q.write32(0x20300000, 1)
        self.wait(q, 0x20300004)
        time.sleep(.03)
        self.assertEqual([q.read32(a) for a in markers], [0, 0, 0])
        for stage in (2, 3, 4):
            q.write32(0x20300000, stage)
            self.wait(q, markers[stage-2])
            self.assertEqual([q.read32(a) for a in markers],
                             [int(i < stage-1) for i in range(3)])
        cores = q.qmp('query-cpus-fast')
        self.assertEqual(len({c['thread-id'] for c in cores}), 4)
        q.qmp('system_reset')
        q.qmp('query-status')
        self.assertEqual(q.read32(0x400800cc) & 0x13c8, 0)
        self.assertEqual(q.read32(0x400801a4) & 2, 2)

    def wait(self, q, address):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            if q.read32(address) == 1:
                return
            time.sleep(.005)
        self.fail(f'peer failed to execute after AP release: {address:#x}')
