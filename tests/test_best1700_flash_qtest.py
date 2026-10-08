"""NOR controller transactions against isolated BEST1700 QEMU instances."""
import os
from pathlib import Path
import struct
import tempfile
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


BASE = 0x40190000
CMD, CTRL, TX, STATUS, RX, CLEAR = 0, 4, 8, 12, 16, 24
DUAL = 0x204
TX_COPY = 1 << 21


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700FlashQtest(unittest.TestCase):
    def setUp(self):
        self.q = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"])
        self.addCleanup(self.q.close)

    def status(self):
        return self.q.read32(BASE + STATUS)

    def transmit(self, data):
        for byte in data:
            self.q.command(f"writeb {BASE + TX:#x} {byte:#x}")

    def drain(self, count):
        return bytes(self.q.read32(BASE + RX) for _ in range(count))

    def ext(self, opcode, tx=b"", rx=0):
        q = self.q
        q.write32(BASE + CLEAR, 3)
        self.transmit(tx)
        q.write32(BASE + CTRL, 1 | (len(tx) << 12))
        q.write32(BASE + CMD, opcode | (0x100 | (rx << 13) if rx else 0))
        self.assertTrue(self.status() & 1)
        q.step(1000)
        self.assertFalse(self.status() & 1)
        return self.drain(rx)

    def normal(self, opcode, address=0):
        self.q.write32(BASE + CTRL, self.q.read32(BASE + CTRL) & ~1)
        self.q.write32(BASE + 0x30, address >> 24)
        self.q.write32(BASE + CMD, opcode | ((address & 0xffffff) << 8))
        self.q.step(1000)
        self.assertFalse(self.status() & 1)

    def test_fifo_counts_clear_and_receive_lifetime(self):
        q = self.q
        self.assertEqual(self.status(), (16 << 9) | 8)
        for remaining in range(15, -1, -1):
            self.transmit(bytes([remaining]))
            self.assertEqual((self.status() >> 9) & 31, remaining)
        q.write32(BASE + CLEAR, 1)
        self.assertEqual((self.status() >> 9) & 31, 0)
        q.write32(BASE + CLEAR, 2)
        self.assertEqual((self.status() >> 9) & 31, 16)
        q.write32(BASE + CTRL, 1)
        q.write32(BASE + CMD, 0x9f | 0x100 | (3 << 13))
        self.assertEqual(self.status() & 9, 9)
        q.step(1000)
        self.assertEqual((self.status() >> 4) & 31, 3)
        self.assertEqual(q.read32(BASE + RX), 0xc8)
        self.assertEqual((self.status() >> 4) & 31, 2)
        q.write32(BASE + CLEAR, 2)
        self.assertEqual(self.drain(2), b"\x60\x17")
        self.assertEqual(self.status() & 9, 8)

    def test_single_die_power_broadcast_preserves_dual_configuration(self):
        q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                         machine_options=',nor-die-count=1')
        self.addCleanup(q.close)
        q.write32(BASE + DUAL, 1 << 19)
        # Normal command-only opcodes ignore a previous data block length.
        q.write32(BASE + CTRL, 2 << 12)
        for opcode in (0xb9, 0xab):
            q.write32(BASE + CMD, opcode)
            self.assertTrue(q.read32(BASE + STATUS) & 1)
            q.step(1000)
            self.assertFalse(q.read32(BASE + STATUS) & 1)
            self.assertEqual(q.read32(BASE + DUAL), 1 << 19)
        # Actual two-lane data requests must still reject the absent die.
        q.write32(BASE + CTRL, 1)
        q.write32(BASE + CMD, 0x9f | 0x100 | (6 << 13))
        q.step(1000)
        self.assertTrue(q.read32(BASE + STATUS) & 1)
        self.assertIn('dual mode requires two configured dies', q.stderr_path.read_text())

    def test_single_and_dual_jedec_and_long_uid_receive(self):
        q = self.q
        self.assertEqual(self.ext(0x9f, rx=3), b"\xc8\x60\x17")
        q.write32(BASE + DUAL, 1 << 19)
        self.assertEqual(self.ext(0x9f, rx=6), b"\xcc\x88\x66\x00\x11\x77")
        q.write32(BASE + CLEAR, 3)
        self.transmit(bytes(4))
        q.write32(BASE + CTRL, 1 | (4 << 12))
        q.write32(BASE + CMD, 0x4b | 0x100 | (36 << 13))
        q.step(1000)
        self.assertEqual((self.status() >> 4) & 31, 16)
        result = self.drain(36)
        identities = [[], []]
        for first, second in zip(result[::2], result[1::2]):
            identities[0].append(((first & 15) << 4) | (second & 15))
            identities[1].append((first & 0xf0) | (second >> 4))
        for index in range(2):
            seed = (0x1700000000000001 + index).to_bytes(8, "little")
            self.assertEqual(bytes(identities[index]),
                             bytes(seed[i % 8] ^ (0x31 * (i // 8)) for i in range(18)))
        self.assertEqual(self.status() & 9, 8)

    def test_status_write_enable_qe_reset_and_dual_copy(self):
        q = self.q
        q.write32(BASE + DUAL, (1 << 19) | TX_COPY)
        self.normal(0x06)
        self.assertEqual(self.ext(0x05, rx=2), b"\x00\x22")
        self.ext(0x31, tx=b"\x02")
        q.step(10000)
        self.assertEqual(self.ext(0x35, rx=2), b"\x00\x22")
        self.assertEqual(self.ext(0x05, rx=2), b"\x00\x00")
        self.normal(0x66)
        self.normal(0x99)
        self.assertEqual(self.ext(0x35, rx=2), b"\x00\x22")
        self.normal(0x50)
        self.ext(0x31, tx=b"\x00")
        q.step(10000)
        self.assertEqual(self.ext(0x35, rx=2), b"\x00\x00")
        self.normal(0x66)
        self.normal(0x99)
        self.assertEqual(self.ext(0x35, rx=2), b"\x00\x22")

    def test_security_read_normal_frame_single_and_dual(self):
        q = self.q
        original = q.read(0x10003000, 32)
        for dual in (False, True):
            q.write32(BASE + DUAL, (1 << 19) if dual else 0)
            self.normal(0x06)
            # Security addresses are die addresses even in dual mode.
            # Dividing 0x3000 by two would incorrectly select reserved 0x1800.
            for address, length in ((0x1000, 16), (0x2000, 32),
                                    (0x3000, 16), (0x3010, 16),
                                    (0x33ff, 2 if dual else 1)):
                q.write32(BASE + CLEAR, 3)
                q.write32(BASE + CTRL, length << 12)
                q.write32(BASE + CMD, (address << 8) | 0x48)
                self.assertEqual(self.status() & 9, 9)
                q.step(999)
                self.assertTrue(self.status() & 1)
                q.step(1)
                self.assertFalse(self.status() & 1)
                self.assertEqual((self.status() >> 4) & 31, min(length, 16))
                self.assertEqual(self.drain(length), b"\xff" * length)
                self.assertEqual(self.status() & 9, 8)
            self.assertEqual(self.ext(0x05, rx=2 if dual else 1),
                             b"\x00\x22" if dual else b"\x02")
        self.assertEqual(q.read(0x10003000, 32), original)
        q.qmp("system_reset")
        q.qmp("query-status")
        q.write32(BASE + CTRL, 16 << 12)
        self.normal(0x48, 0x3000)
        self.assertEqual(self.drain(16), b"\xff" * 16)

    def test_security_read_invalid_frames_reject_without_stale_fifo(self):
        q = self.q
        for address, length in ((0x3000, 0), (0x33ff, 2), (0x3400, 1),
                                (0, 16), (0x4000, 16)):
            q.write32(BASE + CLEAR, 3)
            q.write32(BASE + CTRL, length << 12)
            q.write32(BASE + CMD, (address << 8) | 0x48)
            q.step(1000)
            self.assertEqual(self.status() & 9, 9)
            self.assertEqual((self.status() >> 4) & 31, 0)
        q.write32(BASE + CLEAR, 3)
        q.write32(BASE + CTRL, 1)
        q.write32(BASE + CMD, 0x48 | 0x100 | (16 << 13))
        q.step(1000)
        self.assertEqual(self.status() & 9, 9)
        q.write32(BASE + CLEAR, 3)
        q.write32(BASE + CTRL, 16 << 12)
        self.normal(0x48, 0x3000)
        self.assertEqual(self.drain(16), b"\xff" * 16)

    def test_broadcast_protection_leaves_upper_package_storage_writable(self):
        q = self.q
        # A real register transaction sequence: dual enable plus TX copy.
        # These are ordinary status bytes, not interleaved wire nibbles.
        q.write32(BASE + DUAL, 0x0028000a)
        self.normal(0x50)
        self.ext(0x01, tx=b"\x08\x40")
        q.step(10000)
        self.assertEqual(self.ext(0x05, rx=2), b"\x00\x88")
        self.assertEqual(self.ext(0x35, rx=2), b"\x44\x00")
        q.write32(BASE + DUAL, 0x0008000a)

        # Each 8 MiB die protects all but its upper 256 KiB; the aggregate
        # package therefore leaves its upper 512 KiB writable.
        address = 0xf8e000
        payload = b"\x12\x34\x56\x78"
        self.transmit(payload)
        q.write32(BASE + CTRL, len(payload) << 12)
        self.normal(0x06)
        self.normal(0x02, address)
        q.step(50000)
        self.assertEqual(q.read(0x10000000 + address, len(payload)), payload)
        self.normal(0x06)
        self.normal(0x20, address)
        q.step(200000)
        self.assertEqual(q.read(0x10000000 + address, len(payload)), b"\xff" * 4)

        # A block erase covering protected bytes must still fail atomically.
        self.normal(0x06)
        original = q.read(0x101c0000, 32)
        q.write32(BASE + CMD, 0x1c0000d8)
        q.step(1000)
        self.assertTrue(self.status() & 1)
        self.assertEqual(q.read(0x101c0000, 32), original)
        q.write32(BASE + CLEAR, 3)
        self.assertEqual(self.ext(0x05, rx=2), b"\x00\xaa",
                         "a rejected protected erase must preserve both WEL bits")

    def test_dual_status_without_copy_and_atomic_rejection(self):
        q = self.q
        q.write32(BASE + DUAL, 1 << 19)
        self.normal(0x06)
        self.ext(0x31, tx=b"\x00\x20")  # QE clear in die 0, set in die 1.
        q.step(10000)
        self.assertEqual(self.ext(0x35, rx=2), b"\x00\x20")
        q.write32(BASE + DUAL, 0)
        self.assertEqual(self.ext(0x35, rx=1), b"\x00")
        self.normal(0x06)  # WEL only in die 0.
        q.write32(BASE + DUAL, 1 << 19)
        original = q.read(0x101d0000, 2)
        self.transmit(b"\x00\x00")
        q.write32(BASE + CTRL, 2 << 12)
        q.write32(BASE + CMD, 0x02 | (0x1d0000 << 8))
        q.step(1000)
        self.assertTrue(self.status() & 1)
        self.assertEqual(q.read(0x101d0000, 2), original,
                         "die 1 rejecting a write must not partially program die 0")
        q.write32(BASE + CLEAR, 3)
        q.write32(BASE + DUAL, 0)
        self.assertEqual(self.ext(0x05, rx=1), b"\x02",
                         "failed multi-die validation must preserve die 0 WEL")

    def test_preloaded_fifo_survives_wren_and_program_updates_xip(self):
        q = self.q
        q.write32(BASE + DUAL, 1 << 19)
        address = 0x1d0000
        self.normal(0x06)
        self.normal(0x20, address)
        q.step(200000)
        self.assertEqual(q.read(0x10000000 + address, 64), b"\xff" * 64)
        payload = bytes((i * 37 + 9) & 255 for i in range(48))
        q.write32(BASE + CLEAR, 3)
        self.transmit(payload[:16])
        q.write32(BASE + CTRL, len(payload) << 12)
        self.normal(0x06)
        self.assertEqual((self.status() >> 9) & 31, 0,
                         "WREN must not consume preloaded program data")
        q.write32(BASE + CMD, 0x32 | (address << 8))
        self.assertTrue(self.status() & 1)
        self.assertEqual((self.status() >> 9) & 31, 16)
        self.transmit(payload[16:])
        q.step(1000)
        self.assertFalse(self.status() & 1)
        self.assertEqual(q.read(0x10000000 + address, len(payload)), payload)
        q.step(50000)
        q.write32(BASE + CTRL, len(payload) << 12)
        self.normal(0x03, address)
        self.assertEqual(self.drain(len(payload)), payload)
        # Odd aggregate addresses retain the other half of each die pair.
        q.step(50000)
        self.normal(0x06)
        self.normal(0x20, address)
        q.step(200000)
        self.transmit(b"\x12\x34\x56")
        q.write32(BASE + CTRL, 3 << 12)
        self.normal(0x06)
        self.normal(0x02, address + 1)
        self.assertEqual(q.read(0x10000000 + address, 5), b"\xff\x12\x34\x56\xff")

    def test_rejected_commands_and_reset_have_no_stale_response(self):
        q = self.q
        q.write32(BASE + CTRL, 1)
        q.write32(BASE + CMD, 0xde)
        q.step(1000)
        self.assertEqual(self.status() & 9, 9)
        self.assertEqual((self.status() >> 4) & 31, 0)
        q.write32(BASE + CLEAR, 3)
        self.assertEqual(self.ext(0x9f, rx=3), b"\xc8\x60\x17")
        q.write32(BASE + CTRL, 1)
        q.write32(BASE + CMD, 0x9f | 0x100 | (3 << 13))
        q.qmp("system_reset")
        q.qmp("query-status")
        q.step(10000)
        self.assertEqual(self.status(), (16 << 9) | 8)
        self.assertEqual(self.ext(0x9f, rx=3), b"\xc8\x60\x17")

    def test_zero_length_dual_program_rejects_before_odd_address_padding(self):
        q = self.q
        q.write32(BASE + DUAL, 1 << 19)
        original = q.read(0x101d0000, 4)
        for address in (0x1d0000, 0x1d0001):
            with self.subTest(address=hex(address)):
                q.write32(BASE + CLEAR, 3)
                q.write32(BASE + CTRL, 0)
                self.normal(0x06)
                q.write32(BASE + CMD, 0x02 | (address << 8))
                q.step(1000)
                self.assertTrue(self.status() & 1,
                                "empty page program must be rejected for both address parities")
                self.assertEqual(q.read(0x101d0000, 4), original)
                q.write32(BASE + CLEAR, 3)
                self.assertEqual(self.ext(0x05, rx=2), b"\x00\x22",
                                 "rejected empty program must preserve WEL and leave WIP clear")

    def test_dual_program_wrap_preserves_unwritten_half_and_end_of_die(self):
        q = self.q
        q.write32(BASE + DUAL, 1 << 19)

        def erase(address):
            self.normal(0x06)
            self.normal(0x20, address)
            q.step(200000)

        def program(address, data):
            q.write32(BASE + CLEAR, 3)
            self.transmit(data)
            q.write32(BASE + CTRL, len(data) << 12)
            self.normal(0x06)
            self.normal(0x02, address)
            q.step(50000)

        erase(0x1d0000)
        program(0x1d0201, b"\x00")
        program(0x1d01ff, b"\x12\x34")
        self.assertEqual(q.read(0x101d0000, 4), b"\x34\xff\xff\xff",
                         "wrapped half-pair must preserve this page's byte, not the next page")
        self.assertEqual(q.read(0x101d01fe, 4), b"\xff\x12\xff\x00")
        erase(0xfffffe)
        program(0xffffff, b"\x12\x34")
        self.assertEqual(q.read(0x10fffffe, 2), b"\xff\x12")
        self.assertEqual(q.read(0x10fffe00, 4), b"\x34\xff\xff\xff",
                         "page wrapping at capacity must not be treated as out-of-range")


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700FlashSecureTcg(unittest.TestCase):
    def test_program_invalidates_executed_main_and_alias_translations(self):
        from tests.test_best1700_secure_tcg import Best1700SecureTcg

        assembly = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
vectors:
    .word 0x2415f000
    .word Reset_Handler
    .rept 142
    .word Fault_Handler
    .endr
    .org 0x400
.thumb_func
Reset_Handler:
    ldr r5, =0x24002000
    ldr r1, =0xe000ed08
    ldr r0, =vectors
    str r0, [r1]
    // Populate three TCG translations for the same physical NOR bytes.
    ldr r6, =0x100c0901
    blx r6
    str r0, [r5, #4]
    ldr r7, =0x080c0901
    blx r7
    str r0, [r5, #8]
    ldr r8, =0x300c0901
    blx r8
    str r0, [r5, #12]

    ldr r4, =0x44190000
    movs r0, #3
    str r0, [r4, #24]
    ldr r0, =0x00080000
    str r0, [r4, #0x204]
    movs r0, #0
    str r0, [r4, #4]
    movs r0, #6
    str r0, [r4]
    bl Wait_Controller
    // MOVS r0,#1 -> MOVS r0,#0 changes only a single 1 bit to 0.
    ldr r0, =0x47702000
    str r0, [r4, #8]
    ldr r0, =0x4000
    str r0, [r4, #4]
    ldr r0, =0x0c090002
    str r0, [r4]
    bl Wait_Controller
    dsb sy
    isb sy

    blx r6
    str r0, [r5, #16]
    blx r7
    str r0, [r5, #20]
    blx r8
    str r0, [r5, #24]
    movs r0, #1
    str r0, [r5]
2:
    wfi
    b 2b
.thumb_func
Wait_Controller:
    ldr r0, [r4, #12]
    tst r0, #1
    bne Wait_Controller
    bx lr
.thumb_func
Fault_Handler:
    ldr r5, =0x24002000
    movs r0, #255
    str r0, [r5]
    b 2b
    .org 0x900
.thumb_func
Flash_Function:
    movs r0, #1
    bx lr
"""
        with tempfile.TemporaryDirectory(prefix="best1700-nor-xip-") as tmp:
            secure = Best1700SecureTcg.assemble(self, Path(tmp), "nor-xip",
                                               assembly, 0x100c0000)
            original = secure.read_bytes()
            q = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"],
                machine_options=(",secure-boot=on,boot-sp=0x2415f000,boot-pc=0x100c0401"
                                 f",tee-image={secure},tee-flash-base=0x10000000"
                                 ",tee-code-offset=0xc0000,tee-entry=0x100c0401"),
                accel="tcg", extra_args=("-S", "-icount", "shift=0,sleep=off"))
            self.addCleanup(q.close)
            self.assertEqual(q.read(0x101d0000, 16), b"\xff" * 16,
                             "unloaded NOR must begin erased")
            q.qmp("cont")
            deadline = time.monotonic() + 5
            result = (0,) * 7
            while time.monotonic() < deadline:
                result = struct.unpack("<7I", q.read(0x20002000, 28))
                if result[0]:
                    break
                time.sleep(0.01)
            q.qmp("stop")
            self.assertEqual(result[0], 1, q.diagnostic("NOR XIP reexecution"))
            self.assertEqual(result[1:4], (1, 1, 1))
            self.assertEqual(result[4:7], (0, 0, 0),
                             "program must invalidate all already-executed aliases")
            self.assertEqual(q.read(0x100c0900, 4), b"\x00\x20\x70\x47")
            self.assertEqual(secure.read_bytes(), original,
                             "program must not overwrite the source image")
            q.qmp("system_reset")
            q.qmp("query-status")
            self.assertEqual(q.read(0x100c0900, 4), b"\x00\x20\x70\x47",
                             "machine reset must retain accepted NOR modifications")

    def test_secure_cpu_alias_uses_the_same_fifo(self):
        from tests.test_best1700_secure_tcg import Best1700SecureTcg

        assembly = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
vectors:
    .word 0x2415f000
    .word Reset_Handler
    .rept 142
    .word Fault_Handler
    .endr
    .org 0x400
.thumb_func
Reset_Handler:
    ldr r4, =0x44190000
    ldr r5, =0x24002000
    ldr r1, =0xe000ed08
    ldr r0, =vectors
    str r0, [r1]
    movs r0, #3
    str r0, [r4, #24]
    movs r0, #1
    str r0, [r4, #4]
    ldr r0, =0x619f
    str r0, [r4]
1:
    ldr r0, [r4, #12]
    tst r0, #1
    bne 1b
    str r0, [r5, #4]
    ldr r0, [r4, #16]
    str r0, [r5, #8]
    ldr r0, [r4, #16]
    str r0, [r5, #12]
    ldr r0, [r4, #16]
    str r0, [r5, #16]
    movs r0, #0x5a
    strb r0, [r4, #8]
    ldr r0, [r4, #12]
    str r0, [r5, #20]
    movs r0, #1
    str r0, [r5]
2:
    wfi
    b 2b
.thumb_func
Fault_Handler:
    ldr r5, =0x24002000
    movs r0, #255
    str r0, [r5]
    b 2b
"""
        with tempfile.TemporaryDirectory(prefix="best1700-nor-secure-") as tmp:
            secure = Best1700SecureTcg.assemble(self, Path(tmp), "nor-secure",
                                               assembly, 0x100c0000)
            q = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"],
                machine_options=(",secure-boot=on,boot-sp=0x2415f000,boot-pc=0x100c0401"
                                 f",tee-image={secure},tee-flash-base=0x10000000"
                                 ",tee-code-offset=0xc0000,tee-entry=0x100c0401"),
                accel="tcg", extra_args=("-S", "-icount", "shift=0,sleep=off"))
            self.addCleanup(q.close)
            q.qmp("cont")
            deadline = time.monotonic() + 5
            result = (0,) * 6
            while time.monotonic() < deadline:
                result = struct.unpack("<6I", q.read(0x20002000, 24))
                if result[0]:
                    break
                time.sleep(0.01)
            q.qmp("stop")
            self.assertEqual(result[0], 1, q.diagnostic("secure NOR completion"))
            self.assertEqual(result[1], (16 << 9) | (3 << 4))
            self.assertEqual(result[2:5], (0xc8, 0x60, 0x17))
            self.assertEqual(result[5], (15 << 9) | 8)
            self.assertEqual(q.read32(BASE + STATUS), result[5])
            q.write32(BASE + CLEAR, 2)
            self.assertEqual(q.read32(BASE + STATUS), (16 << 9) | 8)


if __name__ == "__main__":
    unittest.main()
