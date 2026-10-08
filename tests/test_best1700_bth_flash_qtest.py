"""Exercise the independent BTH NOR controller and its shared XIP views.

Each test owns a synthetic QEMU instance. Register transactions use the same
normal/extended framing as the SYS NOR controller contract tests.
"""
import os
import struct
import time
import unittest

from tests import test_best1700_bth_qtest as bth_fixture
from tests.test_best1700_flash_qtest import CMD, CTRL, TX, STATUS, RX, CLEAR
from tests.test_best1700_peripherals_qtest import QtestMachine


BTH = 0x50140000
SYS = 0x40190000
ALIASES = (0x28000000, 0x2c000000, 0x0c000000)
ADDRESS = 0x00200000
UID = 0x1700000000000101
RESULT = 0x23001000
OPTIONS = (',bth-real-boot=on,bth-nor-die-count=1,bth-nor-jedec-id=0xc86018'
           f',bth-nor-uid0={UID},bth-nor-uid1={UID + 1}')


class FlashPort:
    def __init__(self, case, q, base):
        self.case, self.q, self.base = case, q, base

    def status(self):
        return self.q.read32(self.base + STATUS)

    def transmit(self, data):
        for byte in data:
            self.q.command(f'writeb {self.base + TX:#x} {byte:#x}')

    def drain(self, count):
        return bytes(self.q.read32(self.base + RX) for _ in range(count))

    def ext(self, opcode, tx=b'', rx=0):
        self.q.write32(self.base + CLEAR, 3)
        self.transmit(tx)
        self.q.write32(self.base + CTRL, 1 | (len(tx) << 12))
        self.q.write32(self.base + CMD, opcode | (0x100 | (rx << 13) if rx else 0))
        self.case.assertTrue(self.status() & 1)
        self.q.step(1000)
        self.case.assertFalse(self.status() & 1)
        return self.drain(rx)

    def normal(self, opcode, address=0):
        self.q.write32(self.base + CTRL, self.q.read32(self.base + CTRL) & ~1)
        self.q.write32(self.base + 0x30, address >> 24)
        self.q.write32(self.base + CMD, opcode | ((address & 0xffffff) << 8))
        self.q.step(1000)
        self.case.assertFalse(self.status() & 1)

    def program(self, address, data):
        self.q.write32(self.base + CLEAR, 3)
        self.transmit(data)
        self.q.write32(self.base + CTRL, len(data) << 12)
        self.normal(0x06)
        self.normal(0x02, address)
        self.q.step(50000)

    def read(self, address, count):
        self.q.write32(self.base + CLEAR, 3)
        self.q.write32(self.base + CTRL, count << 12)
        self.normal(0x03, address)
        return self.drain(count)


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class BthFlashQtest(unittest.TestCase):
    def machine(self, tcg=False):
        options = OPTIONS
        if tcg:
            options += ',boot-sp=0x203ff000,boot-pc=0x101c0001'
        q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
            image_payload=b'\xfe\xe7' + bytes(32) if tcg else None,
            machine_options=options, accel='tcg' if tcg else 'qtest',
            extra_args=('-S', '-icount', 'shift=3,sleep=off') if tcg else ())
        self.addCleanup(q.close)
        return q

    def test_fifo_jedec_and_uid_are_independent_from_sys_nor(self):
        q = self.machine()
        bth, sys = FlashPort(self, q, BTH), FlashPort(self, q, SYS)
        self.assertEqual(bth.status(), (16 << 9) | 8)
        for remaining in range(15, -1, -1):
            bth.transmit(bytes([remaining]))
            self.assertEqual((bth.status() >> 9) & 31, remaining)
            self.assertEqual(sys.status(), (16 << 9) | 8)
        q.write32(BTH + CLEAR, 1)
        self.assertEqual((bth.status() >> 9) & 31, 0)
        q.write32(BTH + CLEAR, 2)
        self.assertEqual(bth.status(), (16 << 9) | 8)
        q.write32(BTH + CTRL, 1)
        q.write32(BTH + CMD, 0x9f | 0x100 | (3 << 13))
        self.assertTrue(bth.status() & 1)
        q.step(999)
        self.assertTrue(bth.status() & 1)
        q.step(1)
        self.assertEqual((bth.status() >> 4) & 31, 3)
        self.assertEqual(q.read32(BTH + RX), 0xc8)
        self.assertEqual((bth.status() >> 4) & 31, 2)
        q.write32(BTH + CLEAR, 2)
        self.assertEqual(bth.drain(2), b'\x60\x18')
        self.assertEqual(sys.ext(0x9f, rx=3), b'\xc8\x60\x17')
        self.assertEqual(bth.ext(0x9f, rx=3), b'\xc8\x60\x18')
        seed = UID.to_bytes(8, 'little')
        self.assertEqual(bth.ext(0x4b, tx=bytes(4), rx=18),
                         bytes(seed[i % 8] ^ (0x31 * (i // 8)) for i in range(18)))
        self.assertEqual(q.qmp('qom-get', {
            'path': '/machine', 'property': 'bth-nor-jedec-id'}), 0xc86018)
        self.assertEqual(q.qmp('qom-get', {
            'path': '/machine', 'property': 'nor-jedec-id'}), 0xc86017)

    def test_program_and_erase_update_all_bth_aliases_without_touching_sys(self):
        q = self.machine()
        bth, sys = FlashPort(self, q, BTH), FlashPort(self, q, SYS)
        before = q.read(0x10000000 + ADDRESS, 16)
        payload = bytes.fromhex('123456789abcdef00f1e2d3c4b5a6978')
        bth.program(ADDRESS, payload)
        for base in ALIASES:
            self.assertEqual(q.read(base + ADDRESS, len(payload)), payload)
        self.assertEqual(bth.read(ADDRESS, len(payload)), payload)
        self.assertEqual(q.read(0x10000000 + ADDRESS, 16), before)
        # A SYS command at the same die offset targets distinct storage.
        sys_payload = bytes.fromhex('55aa00ff0123456789abcdef10203040')
        sys.program(ADDRESS, sys_payload)
        self.assertEqual(sys.read(ADDRESS, len(sys_payload)), sys_payload)
        self.assertEqual(bth.read(ADDRESS, len(payload)), payload)
        bth.normal(0x06)
        bth.normal(0x20, ADDRESS)
        q.step(200000)
        for base in ALIASES:
            self.assertEqual(q.read(base + ADDRESS, len(payload)), b'\xff' * len(payload))
        self.assertEqual(sys.read(ADDRESS, len(sys_payload)), sys_payload)

    def test_machine_reset_clears_fifos_but_preserves_flash_and_identity(self):
        q = self.machine()
        bth = FlashPort(self, q, BTH)
        payload = b'BTH-NOR!'
        bth.program(ADDRESS, payload)
        bth.transmit(b'\x55\xaa')
        q.write32(BTH + CTRL, 1)
        q.write32(BTH + CMD, 0x9f | 0x100 | (3 << 13))
        q.qmp('system_reset')
        q.qmp('query-status')
        q.step(10000)
        self.assertEqual(bth.status(), (16 << 9) | 8)
        self.assertEqual(bth.ext(0x9f, rx=3), b'\xc8\x60\x18')
        self.assertEqual(bth.read(ADDRESS, len(payload)), payload)
        for base in ALIASES:
            self.assertEqual(q.read(base + ADDRESS, len(payload)), payload)

    def test_bth_guest_reads_controller_id_and_publishes_shared_marker(self):
        code = bth_fixture.BthTests.assemble(self, r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x50140000
    ldr r5, =0x23001000
    movs r0, #3
    str r0, [r4, #24]
    movs r0, #1
    str r0, [r4, #4]
    ldr r0, =0x619f
    str r0, [r4]
1:  ldr r0, [r4, #12]
    tst r0, #1
    bne 1b
    str r0, [r5, #4]
    ldr r0, [r4, #16]
    str r0, [r5, #8]
    ldr r0, [r4, #16]
    str r0, [r5, #12]
    ldr r0, [r4, #16]
    str r0, [r5, #16]
    ldr r0, [r4, #12]
    str r0, [r5, #20]
    movs r0, #0xa1
    str r0, [r5]
2:  wfi
    b 2b
    .ltorg
''', base=0x03004000)
        q = self.machine(tcg=True)
        q.write(0x23004000, code)
        q.write(0x23002000, struct.pack('<II', 0x230bff00, 0x03004001))
        q.write32(0x400800b8, 0x23002000)
        q.write32(0x400800cc, 0x300)
        q.qmp('cont')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and q.read32(RESULT) != 0xa1:
            time.sleep(0.01)
        q.qmp('stop')
        result = struct.unpack('<6I', q.read(RESULT, 24))
        self.assertEqual(result[0], 0xa1, q.diagnostic('BTH flash ID read'))
        self.assertEqual(result[1], (16 << 9) | (3 << 4))
        self.assertEqual(result[2:5], (0xc8, 0x60, 0x18))
        self.assertEqual(result[5], (16 << 9) | 8)
        self.assertEqual(q.read32(BTH + STATUS), result[5])


if __name__ == '__main__':
    unittest.main()
