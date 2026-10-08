"""Exercise original BTH DMA initialization in the BTH guest address space."""
import os
import struct
import time
import unittest
from tests import test_best1700_btc_baseband_qtest as baseband_qtest


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BthDmaTests(unittest.TestCase):
    assemble = baseband_qtest.BtcBasebandTests.assemble
    machine = baseband_qtest.BtcBasebandTests.machine

    def test_eight_register_banks_initialize_copy_and_reset_on_both_controllers(self):
        q = self.machine(tcg=True)
        # Failures mark RESULT through the BTH vector table instead of silently
        # spinning at a default exception entry. Controller accesses are guest
        # loads/stores, not AP qtest accesses to an unrelated address space.
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    cpsid i
    ldr r4, =0x23001000
    movs r0, #0
    str r0, [r4]
    ldr r5, =0x50170000
    bl init
    ldr r5, =0x50130000
    bl init
    ldr r0, =0x12345678
    str r0, [r4, #4]
    ldr r5, =0x50170000
    bl copy
    ldr r5, =0x50130000
    bl copy
    ldr r0, =0x600d
    str r0, [r4]
halt:
    wfi
    b halt
init:
    ldr r0, [r5, #0x30]
    cmp r0, #0
    bne fault
    addw r1, r5, #0x110
    movs r2, #8
check:
    ldr r0, [r1]
    cmp r0, #0
    bne fault
    movs r0, #0
    str r0, [r1]
    addw r1, r1, #0x20
    subs r2, #1
    bne check
    mvn r0, #0
    str r0, [r5, #8]
    str r0, [r5, #0x10]
    movs r0, #9
    str r0, [r5, #0x30]
    ldr r0, [r5, #0x30]
    cmp r0, #9
    bne fault
    bx lr
copy:
    addw r1, r4, #4
    str r1, [r5, #0x100]
    addw r1, r4, #8
    str r1, [r5, #0x104]
    movs r0, #0
    str r0, [r5, #0x108]
    ldr r0, =0x8c480001
    str r0, [r5, #0x10c]
    ldr r0, =0x8001
    str r0, [r5, #0x110]
wait_copy:
    ldr r0, [r5, #0x14]
    tst r0, #1
    beq wait_copy
    ldr r0, [r4, #8]
    ldr r1, [r4, #4]
    cmp r0, r1
    bne fault
    movs r0, #1
    str r0, [r5, #8]
    movs r0, #0
    str r0, [r4, #8]
    bx lr
.thumb_func
fault:
    ldr r4, =0x23001000
    ldr r0, =0xfa17
    str r0, [r4]
    b halt
.ltorg
""", base=0x23004000)
        # A bus exception must not be mistaken for a timeout or success.
        for cycle in range(2):
            if cycle:
                q.qmp("system_reset")
                q.qmp("query-status")
            q.write32(0x23001000, 0)
            q.write(0x23004000, code)
            vectors = bytearray(256)
            struct.pack_into("<II", vectors, 0, 0x230bff00, 0x23004001)
            # Install the exception handler at an independent fixed address.
            fault = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x23001000
    ldr r0, =0xfa17
    str r0, [r4]
1:  wfi
    b 1b
.ltorg
""", base=0x23006000)
            q.write(0x23006000, fault)
            for exception in range(2, 16):
                struct.pack_into("<I", vectors, exception * 4, 0x23006001)
            q.write(0x23002000, vectors)
            q.write32(0x400800b8, 0x23002000)
            q.write32(0x400800cc, 0x300)
            q.qmp("cont")
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                result = q.read32(0x23001000)
                if result in (0x600d, 0xfa17):
                    break
                time.sleep(.005)
            q.qmp("stop")
            self.assertEqual(q.read32(0x23001000), 0x600d,
                             q.diagnostic("BTH DMA initialization failed"))


if __name__ == "__main__":
    unittest.main()
