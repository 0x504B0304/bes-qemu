"""Execute a synthetic non-secure M55 program through timer IRQ59 and WFI.

Requires MIBAND_BEST1700_QEMU and GNU Arm binutils. The assembly is a test
program generated in a temporary directory; it contains no product firmware.
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


ASSEMBLY = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global vectors
vectors:
    .word 0x203ff000
    .word Reset_Handler
    .rept 73
    .word Default_Handler
    .endr
    .word Timer_IRQHandler
    .rept 68
    .word Default_Handler
    .endr
    .org 0x400

.thumb_func
Reset_Handler:
    cpsid i
    ldr r4, =0x20001000
    movs r0, #0
    str r0, [r4, #0]
    str r0, [r4, #4]
    str r0, [r4, #8]
    str r0, [r4, #12]
    str r0, [r4, #16]
    str r0, [r4, #20]
    str r0, [r4, #24]
    str r0, [r4, #28]
    ldr r0, =vectors
    ldr r1, =0xe000ed08
    str r0, [r1]
    dsb sy
    isb sy
    ldr r0, =0x08000000
    ldr r1, =0xe000e104
    str r0, [r1]
    ldr r0, [r1]
    str r0, [r4, #16]
    ldr r5, =0x40083020
    movs r0, #2
    str r0, [r5, #8]
    movs r0, #1
    str r0, [r5, #12]
    movw r0, #256
    str r0, [r5]
    movs r0, #0xa3
    str r0, [r5, #8]
    movs r0, #1
    str r0, [r4]
    dsb sy
    wfi
    movs r0, #2
    str r0, [r4]
    ldr r0, [r4, #4]
    str r0, [r4, #12]
    ldr r0, [r5, #16]
    str r0, [r4, #8]
    mrs r0, PRIMASK
    str r0, [r4, #24]
    cpsie i
    isb sy
1:
    ldr r0, [r4, #4]
    cmp r0, #1
    bne 1b
    movs r0, #3
    str r0, [r4]
    cpsid i
2:
    wfi
    b 2b

.thumb_func
Timer_IRQHandler:
    push {r0-r3, lr}
    ldr r0, =0x20001000
    ldr r1, [r0, #4]
    adds r1, #1
    str r1, [r0, #4]
    mrs r1, IPSR
    str r1, [r0, #28]
    ldr r2, =0x40083020
    movs r1, #1
    str r1, [r2, #12]
    ldr r1, [r2, #20]
    str r1, [r0, #20]
    dsb sy
    pop {r0-r3, pc}

.thumb_func
Default_Handler:
    ldr r0, =0x20001000
    mrs r1, IPSR
    str r1, [r0, #28]
    movs r1, #0xff
    str r1, [r0]
3:
    b 3b
    .ltorg
"""


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700IrqTcg(unittest.TestCase):
    def test_nonsecure_irq_enable_wakes_masked_wfi_then_services_irq59(self):
        commands = ("arm-none-eabi-as", "arm-none-eabi-ld", "arm-none-eabi-objcopy")
        for command in commands:
            if shutil.which(command) is None:
                self.skipTest(f"{command} is not installed")
        with tempfile.TemporaryDirectory(prefix="best1700-irq-program-") as temp:
            directory = Path(temp)
            source = directory / "timer-wfi.s"
            obj = directory / "timer-wfi.o"
            elf = directory / "timer-wfi.elf"
            binary = directory / "timer-wfi.bin"
            source.write_text(ASSEMBLY)
            for command in (
                [commands[0], "-march=armv8-m.main", "-mthumb", "-o", str(obj), str(source)],
                [commands[1], "-Ttext=0x101c0000", "-e", "0x101c0401",
                 "-o", str(elf), str(obj)],
                [commands[2], "-O", "binary", str(elf), str(binary)],
            ):
                completed = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(completed.returncode, 0,
                                 f"{command[0]} failed:\n{completed.stderr}")
            image = binary.read_bytes()
        self.assertEqual(struct.unpack_from("<I", image, 4)[0], 0x101C0401)
        machine = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"], image_payload=image,
            machine_options=",boot-sp=0x203ff000,boot-pc=0x101c0401",
            accel="tcg", extra_args=("-S", "-icount", "shift=0,sleep=off"))
        self.addCleanup(machine.close)
        for cycle in ("initial boot", "system reset"):
            with self.subTest(cycle=cycle):
                if cycle == "system reset":
                    machine.qmp("system_reset")
                    machine.qmp("query-status")
                    # RAM survives reset, so discard previous evidence before
                    # letting the same guest establish its IRQ state again.
                    machine.write(0x20001000, bytes(32))
                machine.qmp("cont")
                deadline = time.monotonic() + 5
                result = (0,) * 8
                while time.monotonic() < deadline:
                    result = struct.unpack("<8I", machine.read(0x20001000, 32))
                    if result[0] in (3, 0xFF):
                        break
                    time.sleep(0.01)
                machine.qmp("stop")
                self.assertEqual(result[0], 3, f"guest did not finish: result words={result}")
                self.assertEqual(result[1], 1, "timer ISR must execute exactly once")
                self.assertEqual(result[2], 1, "WFI must resume with the timer pending")
                self.assertEqual(result[3], 0, "PRIMASK must defer servicing until cpsie")
                self.assertEqual(result[4] & (1 << 27), 1 << 27,
                                 "the non-secure guest must be able to enable IRQ59")
                self.assertEqual(result[5], 0, "ISR acknowledgement must clear MIS")
                self.assertEqual(result[6], 1, "WFI must wake while PRIMASK remains set")
                self.assertEqual(result[7], 16 + 59, "the exception must be IRQ59")
                self.assertEqual(machine.read32(0x40083030), 0)
                self.assertEqual(machine.read32(0x40083034), 0)


if __name__ == "__main__":
    unittest.main()
