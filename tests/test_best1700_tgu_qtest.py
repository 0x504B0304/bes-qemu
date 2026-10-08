"""Exercise real M55 secure/NS TCM transactions and IOMMU invalidation.

Requires MIBAND_BEST1700_QEMU and GNU Arm binutils. Synthetic programs,
QEMU processes and logs stay in temporary storage; no firmware is needed.
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
WORDS = 24

SECURE = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global vectors
vectors:
    .word 0x2415f000
    .word reset
    .rept 142
    .word fault
    .endr
    .org 0x400
.thumb_func
reset:
    ldr r4, =0x24001000
    mov r2, r4
    movs r0, #0
    movs r1, #24
1:  str r0, [r2], #4
    subs r1, #1
    bne 1b
    ldr r1, =0xe000ed08
    ldr r0, =vectors
    str r0, [r1]
    ldr r5, =0xe001e000
    ldr r0, [r5, #0x504]
    str r0, [r4, #4]
    ldr r0, [r5, #0x604]
    str r0, [r4, #8]
    // Reset must clear CTRL and LUT state, including on a second boot.
    ldr r0, [r5, #0x500]
    str r0, [r4, #12]
    ldr r0, [r5, #0x538]
    str r0, [r4, #16]
    ldr r0, [r5, #0x628]
    str r0, [r4, #20]
    movs r0, #0
    str r0, [r5, #0x504]
    str r0, [r5, #0x604]
    ldr r0, [r5, #0x504]
    str r0, [r4, #24]
    ldr r0, [r5, #0x604]
    str r0, [r4, #28]
    movs r0, #255
    str r0, [r5, #0x500]
    str r0, [r5, #0x600]
    str r0, [r5, #0x508]
    ldr r0, [r5, #0x500]
    str r0, [r4, #32]
    ldr r0, [r5, #0x600]
    str r0, [r4, #36]
    ldr r0, [r5, #0x508]
    str r0, [r4, #40]

    // ITCM block 320 and DTCM block 192 are independent permissions.
    movs r0, #1
    str r0, [r5, #0x538]
    str r0, [r5, #0x628]
    // The NS stack occupies DTCM block 511.
    ldr r0, =0x80000000
    str r0, [r5, #0x64c]
    ldr r0, =0x4770202a
    ldr r1, =0x04140000
    str r0, [r1]
    ldr r0, =0x12345678
    ldr r1, =0x24180000
    str r0, [r1]

    // Flash MPC starts Secure. Explicitly grant the synthetic AP's
    // 64 KiB code slot to NS without exposing the preceding TEE image.
    ldr r5, =0x44054000
    movs r0, #0
    str r0, [r5]
    ldr r2, [r5, #0x14]
    adds r2, #10
    ldr r0, =0x1c0000
    lsrs r0, r0, r2
    ldr r1, =0x1cffff
    lsrs r1, r1, r2
4:  str r0, [r5, #0x18]
    ldr r3, =0xffffffff
    str r3, [r5, #0x1c]
    adds r0, #1
    cmp r0, r1
    bls 4b
    ldr r5, =0xe001e000

    ldr r6, =0xe000edd0
    movs r0, #0
    str r0, [r6]
    str r0, [r6, #8]
    str r0, [r6, #12]
    ldr r0, =0x0fffffe1
    str r0, [r6, #16]
    movs r0, #1
    str r0, [r6, #8]
    ldr r0, =0x20000000
    str r0, [r6, #12]
    ldr r0, =0x3fffffe1
    str r0, [r6, #16]
    movs r0, #2
    str r0, [r6, #8]
    ldr r0, =0x101c0000
    str r0, [r6, #12]
    ldr r0, =0x101cffe1
    str r0, [r6, #16]
    movs r0, #1
    str r0, [r6]
    ldr r0, =0x101c0000
    ldr r1, =0xe002ed08
    str r0, [r1]
    ldr r0, =0x203ff000
    msr MSP_NS, r0
    dsb sy
    isb sy
    ldr r0, =0x101c0400
    blxns r0
    // The first call populated TCG translations for both TCM ports.
    str r0, [r4, #44]
    str r1, [r4, #48]
    str r2, [r4, #52]
    str r3, [r4, #68]
    str r12, [r4, #72]
    movs r0, #2
    str r0, [r4]
    movs r0, #0
    str r0, [r5, #0x538]
    str r0, [r5, #0x628]
    movs r0, #@CTRL@
    str r0, [r5, #0x500]
    str r0, [r5, #0x600]
    dsb sy
    isb sy
    ldr r0, =0x101c0600
    blxns r0
    movs r0, #0xee
    str r0, [r4]
2:  b 2b
    .ltorg
    .org 0x900
.thumb_func
fault:
    ldr r0, =0x24001000
    mrs r1, IPSR
    str r1, [r0, #56]
    ldr r1, =0xe000ed28
    ldr r1, [r1]
    str r1, [r0, #60]
    ldr r1, =0xe002ed28
    ldr r1, [r1]
    str r1, [r0, #64]
    ldr r1, =0xe000ede4
    ldr r1, [r1]
    str r1, [r0, #76]
    movs r1, #3
    str r1, [r0]
3:  b 3b
    .ltorg
"""

NONSECURE = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global vectors
vectors:
    .word 0x203ff000
    .word first
    .rept 142
    .word failure
    .endr
    .org 0x400
.thumb_func
first:
    push {r4, lr}
    ldr r3, =0x00140001
    blx r3
    ldr r3, =0x21940000
    ldr r3, [r3]
    ldr r4, =0x4770202a
    cmp r3, r4
    bne failure
    ldr r3, =0x20180000
    ldr r1, [r3]
    ldr r3, =0x21180000
    ldr r2, [r3]
    // AP flash remains readable in both aliases next to the secure slot.
    ldr r3, =0x081c0000
    ldr r12, [r3]
    ldr r3, =0x301c0000
    ldr r3, [r3]
    pop {r4, pc}
    .ltorg
    .org 0x600
.thumb_func
second:
    @PROBE@
    bx lr
    .ltorg
.thumb_func
failure:
    b failure
"""


MEMSYSCTL_SECURE = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global vectors
vectors:
    .word 0x2415f000
    .word reset
    .rept 142
    .word fault
    .endr
    .org 0x400
.thumb_func
reset:
    ldr r4, =0x24001000
    mov r2, r4
    movs r0, #0
    movs r1, #24
1:  str r0, [r2], #4
    subs r1, #1
    bne 1b
    ldr r1, =0xe000ed08
    ldr r0, =vectors
    str r0, [r1]
    ldr r5, =0xe001e000
    ldr r0, [r5]
    str r0, [r4, #4]
    ldr r0, [r5, #4]
    str r0, [r4, #8]
    ldr r0, [r5, #0x500]
    str r0, [r4, #12]
    ldr r0, [r5, #0x600]
    str r0, [r4, #16]
    // Exercise reserved and absent ECC fields without depending on reset.
    ldr r0, =0xffffffff
    str r0, [r5]
    ldr r0, [r5]
    str r0, [r4, #20]
    ldr r0, =0xfffffe55
    str r0, [r5, #4]
    ldr r0, [r5, #4]
    str r0, [r4, #24]
    // Only the result block and NS stack are made accessible to NS.
    movs r0, #1
    str r0, [r5, #0x610]
    ldr r0, =0x80000000
    str r0, [r5, #0x64c]

    ldr r5, =0x44054000
    movs r0, #0
    str r0, [r5]
    ldr r2, [r5, #0x14]
    adds r2, #10
    ldr r0, =0x1c0000
    lsrs r0, r0, r2
    ldr r1, =0x1cffff
    lsrs r1, r1, r2
2:  str r0, [r5, #0x18]
    ldr r3, =0xffffffff
    str r3, [r5, #0x1c]
    adds r0, #1
    cmp r0, r1
    bls 2b
    ldr r6, =0xe000edd0
    movs r0, #0
    str r0, [r6]
    str r0, [r6, #8]
    ldr r0, =0x20000000
    str r0, [r6, #12]
    ldr r0, =0x3fffffe1
    str r0, [r6, #16]
    movs r0, #1
    str r0, [r6, #8]
    ldr r0, =0x101c0000
    str r0, [r6, #12]
    ldr r0, =0x101cffe1
    str r0, [r6, #16]
    movs r0, #1
    str r0, [r6]
    ldr r0, =0x101c0000
    ldr r1, =0xe002ed08
    str r0, [r1]
    ldr r0, =0x203ff000
    msr MSP_NS, r0
    ldr r0, =@AIRCR@
    ldr r1, =0xe000ed0c
    str r0, [r1]
    dsb sy
    isb sy
    @SECURE_PROBE@
    ldr r0, =0x101c0400
    blxns r0
    ldr r5, =0xe001e000
    ldr r0, [r5]
    str r0, [r4, #48]
    ldr r0, [r5, #4]
    str r0, [r4, #52]
    ldr r0, [r5, #0x500]
    str r0, [r4, #68]
    ldr r0, [r5, #0x600]
    str r0, [r4, #72]
    ldr r0, [r5, #0x610]
    str r0, [r4, #76]
    ldr r0, [r5, #0x628]
    str r0, [r4, #80]
    movs r0, #0xcc
    str r0, [r4]
3:  b 3b
    .ltorg
    .org 0x900
.thumb_func
fault:
    ldr r0, =0x24001000
    mrs r1, IPSR
    str r1, [r0, #56]
    ldr r1, =0xe000ed28
    ldr r1, [r1]
    str r1, [r0, #60]
    ldr r1, =0xe002ed28
    ldr r1, [r1]
    str r1, [r0, #64]
    movs r1, #3
    str r1, [r0]
4:  b 4b
    .ltorg
"""

MEMSYSCTL_NONSECURE = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global vectors
vectors:
    .word 0x203ff000
    .word first
    .rept 142
    .word fault
    .endr
    .org 0x400
.thumb_func
first:
    push {r4, r5, r6, lr}
    ldr r4, =0x20001000
    ldr r5, =0xe001e000
    @NONSECURE_PROBE@
    ldr r0, [r5]
    str r0, [r4, #28]
    ldr r0, [r5, #4]
    str r0, [r4, #32]
    // Cache and prefetch controls use one shared security state.
    ldr r0, =0xffffdfff
    str r0, [r5]
    ldr r0, =0xfffffdab
    str r0, [r5, #4]
    ldr r0, [r5]
    str r0, [r4, #36]
    ldr r0, [r5, #4]
    str r0, [r4, #40]
    // BFHFNMINS must never expose TGU CTRL/CFG/LUT to NS.
    ldr r0, [r5, #0x500]
    ldr r1, [r5, #0x504]
    orrs r0, r1
    ldr r1, [r5, #0x600]
    orrs r0, r1
    ldr r1, [r5, #0x604]
    orrs r0, r1
    ldr r1, [r5, #0x610]
    orrs r0, r1
    str r0, [r4, #44]
    movs r0, #0
    str r0, [r5, #0x500]
    str r0, [r5, #0x600]
    str r0, [r5, #0x610]
    ldr r0, =0xffffffff
    str r0, [r5, #0x628]
    movs r0, #0x55
    str r0, [r4, #84]
    pop {r4, r5, r6, pc}
    .ltorg
    .org 0x800
.thumb_func
fault:
    ldr r0, =0x20001000
    mrs r1, IPSR
    str r1, [r0, #56]
    ldr r1, =0xe000ed28
    ldr r1, [r1]
    str r1, [r0, #64]
    movs r1, #3
    str r1, [r0]
1:  b 1b
    .ltorg
"""


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700TguQtest(unittest.TestCase):
    def test_default_itcm_axi_and_cpu_share_bytes_both_directions(self):
        q = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"])
        try:
            for cpu, axi in ((0x00140000, 0x21940000),
                             (0x001FFFFC, 0x219FFFFC),
                             (0x00000020, 0x21800020)):
                with self.subTest(cpu=hex(cpu), axi=hex(axi)):
                    q.write32(cpu, 0x1234ABCD)
                    self.assertEqual(q.read32(axi), 0x1234ABCD)
                    q.write32(axi, 0x5678EF01)
                    self.assertEqual(q.read32(cpu), 0x5678EF01)
            q.qmp("system_reset")
            q.qmp("query-status")
            self.assertEqual(q.read32(0x00140000), 0x5678EF01)
            self.assertEqual(q.read32(0x21940000), 0x5678EF01)
        finally:
            q.close()

    def assemble(self, directory, name, source, base):
        commands = [shutil.which(f"arm-none-eabi-{tool}")
                    for tool in ("as", "ld", "objcopy")]
        if not all(commands):
            self.skipTest("GNU Arm binutils are required")
        path = directory / name
        path.with_suffix(".s").write_text(source)
        for command in (
            [commands[0], "-mthumb", "-march=armv8-m.main", "-o",
             str(path.with_suffix(".o")), str(path.with_suffix(".s"))],
            [commands[1], f"-Ttext={base:#x}", "-e", f"{base + 0x401:#x}",
             "-o", str(path.with_suffix(".elf")), str(path.with_suffix(".o"))],
            [commands[2], "-O", "binary", str(path.with_suffix(".elf")),
             str(path.with_suffix(".bin"))],
        ):
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        return path.with_suffix(".bin")

    def run_case(self, probe, ctrl=3, reset=False, attribution_fault=False):
        with tempfile.TemporaryDirectory(prefix="best1700-tgu-") as temp:
            directory = Path(temp)
            secure = self.assemble(directory, "secure",
                                   SECURE.replace("@CTRL@", str(ctrl)), 0x100C0000)
            ap = self.assemble(directory, "ap",
                               NONSECURE.replace("@PROBE@", probe), 0x101C0000)
            q = QtestMachine(
                os.environ["MIBAND_BEST1700_QEMU"], image_payload=ap.read_bytes(),
                machine_options=(",secure-boot=on,boot-sp=0x2415f000"
                                 ",boot-pc=0x100c0401"
                                 f",tee-image={secure},tee-flash-base=0x10000000"
                                 ",tee-code-offset=0xc0000,tee-entry=0x100c0401"),
                accel="tcg", extra_args=("-S", "-icount", "shift=0,sleep=off"))
            try:
                for cycle in range(2 if reset else 1):
                    if cycle:
                        q.qmp("system_reset")
                        q.qmp("query-status")
                        q.write(RESULT, bytes(WORDS * 4))
                    q.qmp("cont")
                    deadline = time.monotonic() + 5
                    values = (0,) * WORDS
                    while time.monotonic() < deadline:
                        values = struct.unpack(f"<{WORDS}I", q.read(RESULT, WORDS * 4))
                        if values[0] in (3, 0xEE):
                            break
                        time.sleep(0.01)
                    q.qmp("stop")
                    self.assertEqual(values[0], 3,
                                     f"No expected TGU fault: {values}\n{q.diagnostic('TGU')}")
                    self.assertEqual(values[1:3], (0x80000907, 0x80000908))
                    self.assertEqual(values[3:6], (3, 0, 0), "reset must revoke NS access")
                    self.assertEqual(values[6:8], values[1:3], "CFG must be read-only")
                    self.assertEqual(values[8:11], (3, 3, 0), "CTRL masks and reserved bits")
                    self.assertEqual(values[11:14], (42, 0x12345678, 0x12345678),
                                     "NS ITCM execution and CPU/AXI DTCM reads must work")
                    self.assertEqual(values[17:19], (0x203FF000, 0x203FF000),
                                     "AP flash must remain accessible through both aliases")
                    if attribution_fault:
                        self.assertIn(values[14], (3, 7), "alias attribution must fault")
                        self.assertNotEqual(values[19] & 8, 0,
                                            "SFSR must record an attribution violation")
                    else:
                        self.assertIn(values[14], (3, 5), "access rejection must fault")
                        self.assertNotEqual((values[15] | values[16]) & 0x300, 0,
                                            "CFSR must record an instruction/data bus error")
                    self.assertEqual(q.read32(0x24180000), 0x12345678,
                                     "rejected write must preserve backing memory")
                    self.assertEqual(q.read32(0x00140000), 0x4770202A,
                                     "rejected ITCM alias write must preserve code")
                    self.assertEqual(q.read32(0x21940000), 0x4770202A)
            finally:
                q.close()

    def test_registers_ns_read_revocation_and_system_reset(self):
        self.run_case("ldr r3, =0x20180000\n    ldr r0, [r3]", reset=True)

    def test_nonsecure_write_rejected_with_ctrl_zero(self):
        self.run_case("ldr r3, =0x20180000\n    movs r0, #7\n    str r0, [r3]", ctrl=0)

    def test_axi_alias_cannot_bypass_revocation(self):
        self.run_case("ldr r3, =0x21180000\n    movs r0, #7\n    str r0, [r3]")

    def test_itcm_execution_permission_revoked_after_translation(self):
        self.run_case("ldr r3, =0x00140001\n    bx r3")

    def test_itcm_axi_alias_respects_permission_revocation(self):
        for instruction in ("ldr r0, [r3]", "str r0, [r3]"):
            with self.subTest(instruction=instruction):
                self.run_case("ldr r3, =0x21940000\n    movs r0, #7\n    " + instruction)

    def test_secure_flash_slot_cannot_be_read_through_nonsecure_aliases(self):
        for address in (0x100C0000, 0x080C0000, 0x300C0000):
            with self.subTest(address=hex(address)):
                self.run_case(f"ldr r3, ={address:#x}\n    ldr r0, [r3]",
                              attribution_fault=True)

    def run_memsysctl_case(self, *, bfhfnmins, secure_probe="",
                          nonsecure_probe="", expect_fault=False, reset=False):
        with tempfile.TemporaryDirectory(prefix="best1700-memsysctl-") as temp:
            directory = Path(temp)
            source = MEMSYSCTL_SECURE.replace("@AIRCR@", hex(0x05FA0000 |
                                                          (bfhfnmins << 13)))
            source = source.replace("@SECURE_PROBE@", secure_probe)
            secure = self.assemble(directory, "secure", source, 0x100C0000)
            source = MEMSYSCTL_NONSECURE.replace("@NONSECURE_PROBE@", nonsecure_probe)
            ap = self.assemble(directory, "ap", source, 0x101C0000)
            q = QtestMachine(
                os.environ["MIBAND_BEST1700_QEMU"], image_payload=ap.read_bytes(),
                machine_options=(",secure-boot=on,boot-sp=0x2415f000"
                                 ",boot-pc=0x100c0401"
                                 f",tee-image={secure},tee-flash-base=0x10000000"
                                 ",tee-code-offset=0xc0000,tee-entry=0x100c0401"),
                accel="tcg", extra_args=("-S", "-icount", "shift=0,sleep=off"))
            try:
                initial = None
                for cycle in range(2 if reset else 1):
                    if cycle:
                        q.qmp("system_reset")
                        q.qmp("query-status")
                        q.write(RESULT, bytes(WORDS * 4))
                    q.qmp("cont")
                    deadline = time.monotonic() + 5
                    values = (0,) * WORDS
                    while time.monotonic() < deadline:
                        values = struct.unpack(f"<{WORDS}I", q.read(RESULT, WORDS * 4))
                        if values[0] in (3, 0xCC):
                            break
                        time.sleep(0.01)
                    q.qmp("stop")
                    self.assertEqual(values[0], 3 if expect_fault else 0xCC,
                                     f"MemSysCtl result: {values}\n{q.diagnostic('MemSysCtl')}")
                    if initial is None:
                        initial = values[1:5]
                    self.assertEqual(values[1:5], initial,
                                     "reset must restore the initial control state")
                    # Verify field access with explicit written values. The
                    # public PFCR summary and field table disagree on reset.
                    self.assertEqual(values[3:7], (3, 3, 0x13004, 0x255))
                    if expect_fault:
                        self.assertIn(values[14], (3, 5))
                        self.assertNotEqual((values[15] | values[16]) & 0x200, 0,
                                            "unprivileged PPB access must BusFault")
                    else:
                        expected = ((0x13004, 0x255, 0x11004, 0x1AB)
                                    if bfhfnmins else (0, 0, 0, 0))
                        self.assertEqual(values[7:11], expected)
                        self.assertEqual(values[11], 0, "NS TGU reads must be RAZ")
                        self.assertEqual(values[12:14], expected[2:] if bfhfnmins
                                         else (0x13004, 0x255),
                                         "NS accesses must obey BFHFNMINS and use shared state")
                        self.assertEqual(values[17:21], (3, 3, 1, 0),
                                         "NS TGU writes must not change permissions or CTRL")
                        self.assertEqual(values[21], 0x55)
            finally:
                q.close()

    def test_nonsecure_memsysctl_is_razwi_when_bfhfnmins_clear(self):
        self.run_memsysctl_case(bfhfnmins=0, reset=True)

    def test_nonsecure_memsysctl_shares_state_when_bfhfnmins_set(self):
        self.run_memsysctl_case(bfhfnmins=1, reset=True)

    def test_unprivileged_memsysctl_and_tgu_accesses_fault(self):
        for secure in (False, True):
            for bfhfnmins in (0, 1):
                for offset in (0, 4, 0x510):
                    for instruction in ("ldr r0, [r1]", "str r0, [r1]"):
                        with self.subTest(secure=secure, bfhfnmins=bfhfnmins,
                                          offset=hex(offset), instruction=instruction):
                            probe = ("movs r0, #1\n    msr CONTROL, r0\n    isb sy\n"
                                     f"    ldr r1, ={0xE001E000 + offset:#x}\n"
                                     f"    {instruction}")
                            self.run_memsysctl_case(
                                bfhfnmins=bfhfnmins, expect_fault=True,
                                secure_probe=probe if secure else "",
                                nonsecure_probe="" if secure else probe)


if __name__ == "__main__":
    unittest.main()
