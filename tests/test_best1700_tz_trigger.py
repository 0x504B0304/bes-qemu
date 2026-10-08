"""Exercise real M55 TrustZone notifications and shared-memory IRQ exchange.

Programs and output stay in temporary storage; no product firmware is used.
MIBAND_BEST1700_QEMU selects the built QEMU with the trigger device connected.
"""

import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


SET = 0x400001E4
CLR = 0x400001E8
IND = 1 << 13
DONE = 1 << 12
RESULT = 0x20001000
WORDS = 15


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700TzTriggerQtest(unittest.TestCase):
    def setUp(self):
        self.q = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"])
        self.addCleanup(self.q.close)
        self.q.command("irq_intercept_in /machine/core[0]")

    def assert_pending(self, bits):
        self.assertEqual(self.q.read32(SET), bits)
        self.assertEqual(self.q.read32(CLR), bits)
        self.assertEqual(self.q.irq(74), bool(bits & IND))
        self.assertEqual(self.q.irq(75), bool(bits & DONE))

    def test_independent_set_clear_and_rearm(self):
        self.assert_pending(0)
        self.q.write32(SET, IND)
        self.assert_pending(IND)
        self.q.write32(SET, DONE)
        self.assert_pending(IND | DONE)
        # Repeated notifications coalesce, and zero is not an acknowledgement.
        self.q.write32(SET, IND)
        self.q.write32(CLR, 0)
        self.assert_pending(IND | DONE)
        self.q.write32(CLR, IND)
        self.assert_pending(DONE)
        self.q.write32(CLR, IND)
        self.assert_pending(DONE)
        self.q.write32(CLR, DONE)
        self.assert_pending(0)
        self.q.write32(SET, IND)
        self.assert_pending(IND)

    def test_reserved_bits_do_not_create_peer_events(self):
        self.q.write32(SET, ~(IND | DONE) & 0xffffffff)
        self.assert_pending(0)
        self.q.write32(SET, DONE)
        self.q.write32(CLR, ~(IND | DONE) & 0xffffffff)
        self.assert_pending(DONE)

    def test_reset_deasserts_both_lines(self):
        self.q.write32(SET, IND | DONE)
        self.assert_pending(IND | DONE)
        self.q.qmp("system_reset")
        self.q.qmp("query-status")
        self.assert_pending(0)


SECURE_ASSEMBLY = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global vectors
vectors:
    .word 0x2415f000
    .word Reset_Handler
    .rept 88
    .word Fault_Handler
    .endr
    .word Secure_IRQHandler
    .rept 53
    .word Fault_Handler
    .endr
    .org 0x400
.thumb_func
Reset_Handler:
    ldr r4, =0x24001000
    movs r0, #0
    movs r1, #15
    mov r2, r4
1:  str r0, [r2], #4
    subs r1, #1
    bne 1b
    ldr r0, =vectors
    ldr r1, =0xe000ed08
    str r0, [r1]
    // Keep the Secure stack protected; share the mailbox and NS stack.
    ldr r1, =0xe001e610
    movs r0, #1
    str r0, [r1]
    ldr r1, =0xe001e64c
    ldr r0, =0x80000000
    str r0, [r1]
    // Open only the AP flash slot using the hardware MPC block geometry.
    ldr r0, =0x44054000
    movs r1, #0
    str r1, [r0]
    ldr r1, [r0, #0x14]
    and r1, r1, #15
    adds r1, #5
    ldr r2, =0x1c0000
    ldr r3, =0x1d0000
    lsrs r2, r2, r1
    lsrs r3, r3, r1
2:  lsrs r1, r2, #5
    str r1, [r0, #0x18]
    ldr r7, [r0, #0x1c]
    and r5, r2, #31
    movs r6, #1
    lsls r6, r6, r5
    orrs r7, r6
    str r7, [r0, #0x1c]
    adds r2, #1
    cmp r2, r3
    blo 2b
    // SAU: AP flash, shared DTCM and ordinary peripheral addresses are NS.
    ldr r5, =0xe000edd0
    movs r0, #0
    str r0, [r5]
    str r0, [r5, #8]
    ldr r0, =0x101c0000
    str r0, [r5, #12]
    ldr r0, =0x101cffe1
    str r0, [r5, #16]
    movs r0, #1
    str r0, [r5, #8]
    ldr r0, =0x20000000
    str r0, [r5, #12]
    ldr r0, =0x203fffe1
    str r0, [r5, #16]
    movs r0, #2
    str r0, [r5, #8]
    ldr r0, =0x40000000
    str r0, [r5, #12]
    ldr r0, =0x43ffffe1
    str r0, [r5, #16]
    movs r0, #1
    str r0, [r5]
    // IRQ74 remains Secure. Only IRQ75 targets Non-secure execution.
    ldr r1, =0xe000e388
    ldr r0, =0x800
    str r0, [r1]
    ldr r1, =0xe000e108
    ldr r0, =0x400
    str r0, [r1]
    ldr r0, =0x101c0000
    ldr r1, =0xe002ed08
    str r0, [r1]
    ldr r0, =0x203ff000
    msr MSP_NS, r0
    // Assert DONE before the NS handler is enabled, through the secure alias.
    ldr r5, =0x440001e4
    ldr r0, [r5]
    str r0, [r4, #48]
    movw r0, #0x1000
    str r0, [r5]
    dsb sy
    isb sy
    ldr r0, =0x101c0400
    blxns r0
3:  wfi
    b 3b
    .ltorg

    .org 0x900
.thumb_func
Secure_IRQHandler:
    push {r0-r3, lr}
    ldr r0, =0x24001000
    ldr r1, [r0, #8]
    adds r1, #1
    str r1, [r0, #8]
    mrs r1, IPSR
    str r1, [r0, #16]
    mrs r1, MSP
    str r1, [r0, #40]
    // The request is supplied by the NS program, not the interrupt model.
    ldr r1, [r0, #20]
    str r1, [r0, #28]
    ldr r1, =0x24681357
    str r1, [r0, #24]
    ldr r2, =0x440001e4
    movw r1, #0x2000
    str r1, [r2, #4]
    movw r1, #0x1000
    str r1, [r2]
    dsb sy
    pop {r0-r3, pc}
    .ltorg

.thumb_func
Fault_Handler:
    ldr r0, =0x24001000
    mrs r1, IPSR
    str r1, [r0, #48]
    ldr r1, =0xe000ed28
    ldr r1, [r1]
    str r1, [r0, #52]
    ldr r1, =0xe000ede4
    ldr r1, [r1]
    str r1, [r0, #56]
    movs r1, #0xff
    str r1, [r0]
4:  b 4b
    .ltorg
"""


NONSECURE_ASSEMBLY = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global vectors
vectors:
    .word 0x203ff000
    .word Reset_Handler
    .rept 89
    .word Fault_Handler
    .endr
    .word Nonsecure_IRQHandler
    .rept 52
    .word Fault_Handler
    .endr
    .org 0x400
.thumb_func
Reset_Handler:
    cpsid i
    ldr r4, =0x20001000
    mrs r0, MSP
    str r0, [r4, #44]
    ldr r0, =0x800
    ldr r1, =0xe000e108
    str r0, [r1]
    movs r0, #1
    str r0, [r4]
    dsb sy
    wfi
    // PRIMASK prevents service but must not prevent pending-IRQ wakeup.
    ldr r0, [r4, #4]
    str r0, [r4, #32]
    ldr r5, =0x400001e4
    ldr r0, [r5]
    str r0, [r4, #36]
    cpsie i
    isb sy
1:  ldr r0, [r4, #4]
    cmp r0, #1
    bne 1b
    ldr r0, =0x13572468
    str r0, [r4, #20]
    dsb sy
    movw r0, #0x2000
    str r0, [r5]
2:  ldr r0, [r4, #4]
    cmp r0, #2
    bne 2b
    ldr r0, [r5]
    str r0, [r4, #52]
    movs r0, #3
    str r0, [r4]
    cpsid i
3:  wfi
    b 3b
    .ltorg

.thumb_func
Nonsecure_IRQHandler:
    push {r0-r3, lr}
    ldr r0, =0x20001000
    ldr r1, [r0, #4]
    adds r1, #1
    str r1, [r0, #4]
    mrs r1, IPSR
    str r1, [r0, #12]
    ldr r2, =0x400001e8
    movw r1, #0x1000
    str r1, [r2]
    dsb sy
    pop {r0-r3, pc}
    .ltorg

.thumb_func
Fault_Handler:
    ldr r0, =0x20001000
    mrs r1, IPSR
    str r1, [r0, #48]
    movs r1, #0xfe
    str r1, [r0]
4:  b 4b
    .ltorg
"""


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700TzTriggerTcg(unittest.TestCase):
    def assemble(self, directory, name, text, base):
        commands = [shutil.which(f"arm-none-eabi-{tool}")
                    for tool in ("as", "ld", "objcopy")]
        if not all(commands):
            self.skipTest("GNU Arm binutils are required")
        path = directory / name
        path.with_suffix(".s").write_text(text)
        for command in (
            [commands[0], "-march=armv8-m.main", "-mthumb", "-o",
             str(path.with_suffix(".o")), str(path.with_suffix(".s"))],
            [commands[1], f"-Ttext={base:#x}", "-e", f"{base + 0x401:#x}",
             "-o", str(path.with_suffix(".elf")), str(path.with_suffix(".o"))],
            [commands[2], "-O", "binary", str(path.with_suffix(".elf")),
             str(path.with_suffix(".bin"))],
        ):
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        return path.with_suffix(".bin")

    def test_secure_nonsecure_request_reply_masked_wfi_and_reset(self):
        with tempfile.TemporaryDirectory(prefix="best1700-tz-trigger-") as temp:
            directory = Path(temp)
            secure = self.assemble(directory, "secure", SECURE_ASSEMBLY, 0x100C0000)
            nonsecure = self.assemble(directory, "nonsecure", NONSECURE_ASSEMBLY,
                                      0x101C0000)
            q = QtestMachine(
                os.environ["MIBAND_BEST1700_QEMU"],
                image_payload=nonsecure.read_bytes(), accel="tcg",
                machine_options=(",secure-boot=on,boot-sp=0x2415f000,boot-pc=0x100c0401"
                                 f",tee-image={secure},tee-flash-base=0x10000000"
                                 ",tee-code-offset=0xc0000,tee-entry=0x100c0401"),
                extra_args=("-S", "-icount", "shift=0,sleep=off"))
            try:
                for cycle in ("boot", "reset"):
                    with self.subTest(cycle=cycle):
                        if cycle == "reset":
                            # Reset with both source latches asserted.
                            q.write32(SET, IND | DONE)
                            q.qmp("system_reset")
                            q.qmp("query-status")
                            q.write(RESULT, bytes(WORDS * 4))
                        q.qmp("cont")
                        deadline = time.monotonic() + 5
                        result = (0,) * WORDS
                        while time.monotonic() < deadline:
                            result = struct.unpack(f"<{WORDS}I", q.read(RESULT, WORDS * 4))
                            if result[0] in (3, 0xFE, 0xFF):
                                break
                            time.sleep(0.01)
                        q.qmp("stop")
                        self.assertEqual(result[0], 3,
                                         f"guest state={result}\n{q.diagnostic('TZ IRQ exchange')}")
                        self.assertEqual(result[1:5], (2, 1, 91, 90),
                                         "IRQ75 must execute NS twice; IRQ74 Secure once")
                        self.assertEqual(result[5:8], (0x13572468, 0x24681357, 0x13572468),
                                         "request and reply must be exchanged by guest handlers")
                        self.assertEqual(result[8:10], (0, DONE),
                                         "WFI must wake before PRIMASK permits the NS handler")
                        self.assertGreaterEqual(result[10], 0x2415EF00)
                        self.assertLess(result[10], 0x2415F000,
                                        "the Secure handler must use the protected Secure stack")
                        self.assertEqual(result[11], 0x203FF000)
                        self.assertEqual(result[12:14], (0, 0),
                                         "reset and guest acknowledgements must clear the latches")
                        self.assertEqual(q.read32(SET), 0)
            finally:
                q.close()


if __name__ == "__main__":
    unittest.main()
