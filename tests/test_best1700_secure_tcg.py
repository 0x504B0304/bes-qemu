"""Execute synthetic M55 secure boot, SAU and SG/BXNS calls under TCG.

Requires MIBAND_BEST1700_QEMU and GNU Arm binutils. Both programs are
assembled in temporary storage; no external firmware or running VM is used.
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


RESULT = 0x20001000
SECURE_SP = 0x2415F000
NONSECURE_SP = 0x203FF000
RESULT_WORDS = 15


SECURE_ASSEMBLY = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global vectors
vectors:
    .word 0x2415f000
    .word Reset_Handler
    .rept 142
    .word Fault_Handler
    .endr
    .org 0x400
.thumb_func
Reset_Handler:
    ldr r4, =0x24001000
    movs r0, #0
    movs r1, #15
    mov r2, r4
1:
    str r0, [r2], #4
    subs r1, #1
    bne 1b
    mrs r0, MSP
    str r0, [r4, #4]
    ldr r0, =vectors
    ldr r1, =0xe000ed08
    str r0, [r1]
    // Enable SecureFault so a prohibited NS load has a precise exception.
    ldr r1, =0xe000ed24
    ldr r0, =0x00080000
    str r0, [r1]

    // Open only the TGU blocks used by the NS test program. ITCM uses
    // 4 KiB blocks, so code at offset 0x100000 is LUT[8] bit 0.
    ldr r1, =0xe001e530
    ldr r0, [r1]
    str r0, [r4, #48]
    movs r0, #1
    str r0, [r1]
    // DTCM uses 8 KiB blocks: result/data are blocks 0 and 1, while the
    // NS stack at 0x203ff000 is in block 511. The secure stack stays closed.
    ldr r1, =0xe001e610
    ldr r0, [r1]
    str r0, [r4, #52]
    movs r0, #3
    str r0, [r1]
    ldr r1, =0xe001e64c
    ldr r0, [r1]
    str r0, [r4, #56]
    ldr r0, =0x80000000
    str r0, [r1]
    dsb sy
    isb sy

    // The flash MPC powers up Secure. Open only the 64 KiB AP test slot;
    // the SG gateway and secure vector table remain in Secure blocks.
    // Read the modeled block geometry instead of assuming a block size.
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
4:
    lsrs r1, r2, #5
    str r1, [r0, #0x18]
    ldr r7, [r0, #0x1c]
    and r5, r2, #31
    movs r6, #1
    lsls r6, r6, r5
    orrs r7, r6
    str r7, [r0, #0x1c]
    adds r2, #1
    cmp r2, r3
    blo 4b
    dsb sy
    isb sy

    // Broad NS regions deliberately include the secure TCM aliases.
    // The machine's IDAU must retain their secure attribution.
    ldr r5, =0xe000edd0
    movs r0, #0
    str r0, [r5]
    str r0, [r5, #8]
    str r0, [r5, #12]
    ldr r0, =0x07ffffe1
    str r0, [r5, #16]
    movs r0, #1
    str r0, [r5, #8]
    ldr r0, =0x20000000
    str r0, [r5, #12]
    ldr r0, =0x27ffffe1
    str r0, [r5, #16]
    movs r0, #2
    str r0, [r5, #8]
    ldr r0, =0x101c0000
    str r0, [r5, #12]
    ldr r0, =0x101cffe1
    str r0, [r5, #16]
    movs r0, #3
    str r0, [r5, #8]
    ldr r0, =0x100c0800
    str r0, [r5, #12]
    ldr r0, =0x100c0803
    str r0, [r5, #16]
    movs r0, #1
    str r0, [r5]
    dsb sy
    isb sy

    // Copy MOVS r0,#42; BX lr into the secure ITCM alias and execute it.
    ldr r0, =0x4770202a
    ldr r1, =0x04100000
    str r0, [r1]
    dsb sy
    isb sy
    adds r1, #1
    blx r1
    str r0, [r4, #8]
    ldr r0, =0xa1b2c3d4
    ldr r1, =0x24002000
    str r0, [r1]

    ldr r0, =0x101c0000
    ldr r1, =0xe002ed08
    str r0, [r1]
    ldr r0, =0x203ff000
    msr MSP_NS, r0
    movs r0, #1
    str r0, [r4]
    dsb sy
    isb sy
    ldr r0, =0x101c0400
    blxns r0

    // A real FNC_RETURN must restore secure execution and its stack.
    ldr r4, =0x24001000
    ldr r1, =0x24002000
    ldr r0, [r1]
    str r0, [r4, #36]
    mrs r0, MSP
    str r0, [r4, #40]
    movs r0, #3
    str r0, [r4]
2:
    wfi
    b 2b
    .ltorg

    .org 0x800
.thumb_func
Gateway:
    sg
    ldr r1, =0x24001000
    str r0, [r1, #24]
    mrs r0, MSP
    str r0, [r1, #32]
    ldr r0, =0x24681357
    bxns lr
    .ltorg

    .org 0x900
.thumb_func
Fault_Handler:
    ldr r0, =0x24001000
    mrs r1, IPSR
    str r1, [r0, #40]
    ldr r1, =0xe000ede4
    ldr r1, [r1]
    str r1, [r0, #44]
    movs r1, #0xff
    str r1, [r0]
3:
    b 3b
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
    .rept 142
    .word Fault_Handler
    .endr
    .org 0x400
.thumb_func
Reset_Handler:
    mrs r0, MSP
    push {r4-r6, lr}
    ldr r4, =0x20001000
    str r0, [r4, #12]
    movs r0, #2
    str r0, [r4]
    ldr r0, =0x00100001
    blx r0
    str r0, [r4, #16]
    ldr r5, =0x20002000
    ldr r0, [r5]
    str r0, [r4, #20]
    ldr r0, =0x13572468
    ldr r1, =0x100c0801
    blx r1
    str r0, [r4, #28]
    // The negative case attempts a secure alias after a successful SG call.
    @SECURE_ALIAS_PROBE@
    ldr r0, =0xbad0cafe
    str r0, [r5]
    pop {r4-r6, lr}
    bx lr
.thumb_func
Fault_Handler:
    ldr r0, =0x20001000
    mrs r1, IPSR
    str r1, [r0, #40]
    movs r1, #0xfe
    str r1, [r0]
1:
    b 1b
    .ltorg
"""


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700SecureTcg(unittest.TestCase):
    def assemble(self, directory, name, assembly, base):
        commands = ("arm-none-eabi-as", "arm-none-eabi-ld", "arm-none-eabi-objcopy")
        for command in commands:
            if shutil.which(command) is None:
                self.skipTest(f"{command} is not installed")
        source = directory / f"{name}.s"
        obj = directory / f"{name}.o"
        elf = directory / f"{name}.elf"
        binary = directory / f"{name}.bin"
        source.write_text(assembly)
        for command in (
            [commands[0], "-march=armv8-m.main", "-mthumb", "-o", str(obj), str(source)],
            [commands[1], f"-Ttext={base:#x}", "-e", f"{base + 0x401:#x}",
             "-o", str(elf), str(obj)],
            [commands[2], "-O", "binary", str(elf), str(binary)],
        ):
            completed = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0,
                             f"{command[0]} failed:\n{completed.stderr}")
        self.assertEqual(struct.unpack_from("<I", binary.read_bytes(), 4)[0],
                         base + 0x401)
        return binary

    def start_machine(self, probe_secure_alias=False):
        temp = tempfile.TemporaryDirectory(prefix="best1700-secure-program-")
        self.addCleanup(temp.cleanup)
        directory = Path(temp.name)
        secure = self.assemble(directory, "secure", SECURE_ASSEMBLY, 0x100C0000)
        probe = "ldr r0, =0x24002000\n    ldr r0, [r0]" if probe_secure_alias else ""
        nonsecure = self.assemble(
            directory, "nonsecure",
            NONSECURE_ASSEMBLY.replace("@SECURE_ALIAS_PROBE@", probe), 0x101C0000)
        machine = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=nonsecure.read_bytes(),
            machine_options=(",secure-boot=on,boot-sp=0x2415f000,boot-pc=0x100c0401"
                             f",tee-image={secure},tee-flash-base=0x10000000"
                             ",tee-code-offset=0xc0000,tee-entry=0x100c0401"),
            accel="tcg", extra_args=("-S", "-icount", "shift=0,sleep=off"))
        self.addCleanup(machine.close)
        return machine, secure.read_bytes(), nonsecure.read_bytes()

    def run_guest(self, machine):
        machine.qmp("cont")
        deadline = time.monotonic() + 5
        result = (0,) * RESULT_WORDS
        while time.monotonic() < deadline:
            result = struct.unpack(f"<{RESULT_WORDS}I", machine.read(RESULT, RESULT_WORDS * 4))
            if result[0] in (3, 0xFE, 0xFF):
                break
            time.sleep(0.01)
        machine.qmp("stop")
        self.assertIn(result[0], (3, 0xFE, 0xFF),
                      f"guest did not finish: {result}\n{machine.diagnostic('guest completion')}")
        return result

    def assert_handoff_and_gateway(self, result):
        self.assertEqual(result[1], SECURE_SP, "reset must start with the secure MSP")
        self.assertEqual(result[2], 42, "secure execution must reach the ITCM alias")
        self.assertEqual(result[3], NONSECURE_SP, "BLXNS must select MSP_NS")
        self.assertEqual(result[4], 42, "AP must execute the same bytes through NS ITCM")
        self.assertEqual(result[5], 0xA1B2C3D4, "AP must see the secure DTCM write")
        self.assertEqual(result[6], 0x13572468, "SG must receive the AP argument")
        self.assertEqual(result[7], 0x24681357, "BXNS must return the service result")
        self.assertGreaterEqual(result[8], SECURE_SP - 32)
        self.assertLessEqual(result[8], SECURE_SP,
                             "the SG gateway must run with the secure stack")
        self.assertEqual(result[12:15], (0, 0, 0),
                         "reset must clear the TGU permissions before guest setup")

    def test_secure_boot_handoff_gateway_aliases_and_reset(self):
        machine, secure, nonsecure = self.start_machine()
        for cycle in ("initial boot", "system reset"):
            with self.subTest(cycle=cycle):
                if cycle == "system reset":
                    machine.qmp("system_reset")
                    machine.qmp("query-status")
                    # RAM survives reset; remove old evidence before re-running.
                    machine.write(RESULT, bytes(RESULT_WORDS * 4))
                    machine.write32(0x20002000, 0)
                    machine.write32(0x00100000, 0)
                self.assertEqual(machine.read(0x100C0000, len(secure)), secure)
                self.assertEqual(machine.read(0x101C0000, len(nonsecure)), nonsecure)
                result = self.run_guest(machine)
                self.assertEqual(result[0], 3, f"secure boot failed: {result}")
                self.assert_handoff_and_gateway(result)
                self.assertEqual(result[9], 0xBAD0CAFE,
                                 "secure code must observe the AP write after return")
                self.assertEqual(result[10], SECURE_SP,
                                 "FNC_RETURN must restore the secure stack")
                self.assertEqual(machine.read32(0x04100000), 0x4770202A)
                self.assertEqual(machine.read32(0x00100000), 0x4770202A)
                self.assertEqual(machine.read32(0x24002000), 0xBAD0CAFE)
                self.assertEqual(machine.read32(0x20002000), 0xBAD0CAFE)

    def test_nonsecure_read_cannot_override_secure_alias_with_sau(self):
        machine, _, _ = self.start_machine(probe_secure_alias=True)
        result = self.run_guest(machine)
        self.assertEqual(result[0], 0xFF, f"NS access must enter SecureFault: {result}")
        self.assert_handoff_and_gateway(result)
        self.assertEqual(result[10], 7, "the prohibited alias read must raise SecureFault")
        self.assertNotEqual(result[11] & 8, 0, "SFSR must report an attribution violation")
        self.assertEqual(machine.read32(0x20002000), 0xA1B2C3D4,
                         "AP must fault before its subsequent store")


if __name__ == "__main__":
    unittest.main()
