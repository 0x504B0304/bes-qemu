"""Exercise BEST1700 MPCs with real Secure/Nonsecure M55 transactions.

Synthetic images and logs remain in temporary storage. Tests inspect guest
results in secure DTCM, not qtest accesses with unspecified security attrs.
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
DONE = 0x600d
FAULT = 0xfa17
FLASH_MPC = 0x44054000
PSRAM_MPC = 0x44056000
BANKS = [(FLASH_MPC, 16 << 20), (PSRAM_MPC, 64 << 20)] + [
    (0x4405a000 + i * 0x2000, 1 << 20) for i in range(8)]


def sau_regions(regions):
    code = "ldr r6, =0xe000edd0\nmovs r0, #0\nstr r0, [r6]\n"
    for index, (start, end, nsc) in enumerate(regions):
        code += (f"movs r0, #{index}\nstr r0, [r6, #8]\n"
                 f"ldr r0, ={start & ~31:#x}\nstr r0, [r6, #12]\n"
                 f"ldr r0, ={(end & ~31) | (3 if nsc else 1):#x}\n"
                 "str r0, [r6, #16]\n")
    return code + "movs r0, #1\nstr r0, [r6]\ndsb sy\nisb sy\n"


def sau_enable(enable):
    return (f"ldr r6, =0xe000edd0\nmovs r0, #{int(enable)}\n"
            "str r0, [r6]\ndsb sy\nisb sy\n")


def lut_word(mpc, offset, value):
    # Geometry comes from the controller, never from a guessed target page.
    return f"""
    ldr r5, ={mpc:#x}
    movs r0, #0
    str r0, [r5]
    ldr r1, [r5, #0x14]
    adds r1, #10
    ldr r0, ={offset:#x}
    lsrs r0, r0, r1
    str r0, [r5, #0x18]
    ldr r0, ={value:#x}
    str r0, [r5, #0x1c]
    dsb sy
    isb sy
"""


def ns_setup(addresses=(), *, gateway=False, control=False):
    code = """
    // The NS stack occupies the final two TGU DTCM blocks.
    ldr r5, =0xe001e000
    ldr r0, =0xc0000000
    str r0, [r5, #0x64c]
    ldr r0, =0x203ff000
    msr MSP_NS, r0
    ldr r0, =0x101c0000
    ldr r1, =0xe002ed08
    str r0, [r1]
"""
    code += lut_word(FLASH_MPC, 0x1c0000, 0xffffffff)
    regions = [(0x101c0000, 0x101cffff, False),
               (0x203fc000, 0x203fffff, False)]
    regions += [(addr & ~31, addr | 31, False) for addr in addresses]
    if gateway:
        regions.append((0x100c3000, 0x100c301f, True))
    if control:
        regions.append((0x40054000, 0x4006bfff, False))
    return code + sau_regions(regions)


def ns_call():
    return "ldr r3, =0x101c0400\nblxns r3\n"


def capture(offset, reg="r0"):
    return f"str {reg}, [r4, #{offset}]\n"


def interrupt_capture(mpc, offset):
    return (f"ldr r5, ={mpc:#x}\nldr r0, [r5, #0x20]\n" + capture(offset) +
            "ldr r0, [r5, #0x2c]\n" + capture(offset + 4) +
            "ldr r0, [r5, #0x30]\n" + capture(offset + 8) +
            "movs r0, #1\nstr r0, [r5, #0x24]\n")


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700MpcQtest(unittest.TestCase):
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

    def run_guest(self, body, ns="bx lr", *, fault=False, cycles=1):
        secure = r"""
.syntax unified
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
    ldr r4, =0x24001000
    mov r2, r4
    movs r0, #0
    movs r1, #128
1:  str r0, [r2], #4
    subs r1, #1
    bne 1b
    ldr r0, =0x100c0000
    ldr r1, =0xe000ed08
    str r0, [r1]
""" + body + r"""
    ldr r0, =0x600d
    str r0, [r4]
2:  wfi
    b 2b
.thumb_func
fault:
    ldr r4, =0x24001000
    ldr r1, =0x44056000
    ldr r0, [r1, #0x20]
    str r0, [r4, #448]
    ldr r0, [r1, #0x2c]
    str r0, [r4, #452]
    ldr r0, [r1, #0x30]
    str r0, [r4, #456]
    mrs r0, IPSR
    str r0, [r4, #480]
    ldr r0, =0xe000ed28
    ldr r0, [r0]
    str r0, [r4, #484]
    ldr r0, =0xe002ed28
    ldr r0, [r0]
    str r0, [r4, #488]
    ldr r0, =0xe000ed38
    ldr r0, [r0]
    str r0, [r4, #496]
    ldr r0, [sp, #24]
    str r0, [r4, #500]
    ldr r0, =0xfa17
    str r0, [r4]
3:  b 3b
.ltorg
.org 0x3000
.thumb_func
gateway:
    sg
    movs r0, #42
    bxns lr
"""
        nonsecure = r"""
.syntax unified
.thumb
.section .text
.word 0x203ff000
.word first
.rept 142
.word failure
.endr
.org 0x400
.thumb_func
first:
""" + ns + "\n.thumb_func\nfailure:\nb failure\n.ltorg\n"
        results = []
        with tempfile.TemporaryDirectory(prefix="best1700-mpc-") as temp:
            directory = Path(temp)
            tee = self.assemble(directory, "tee", secure, 0x100c0000)
            ap = self.assemble(directory, "ap", nonsecure, 0x101c0000)
            q = QtestMachine(
                os.environ["MIBAND_BEST1700_QEMU"], image_payload=ap.read_bytes(),
                accel="tcg", extra_args=("-S", "-icount", "shift=0,sleep=off"),
                machine_options=(",secure-boot=on,boot-sp=0x2415f000"
                                 ",boot-pc=0x100c0401"
                                 f",tee-image={tee},tee-flash-base=0x10000000"
                                 ",tee-code-offset=0xc0000,tee-entry=0x100c0401"))
            try:
                for cycle in range(cycles):
                    if cycle:
                        q.qmp("system_reset")
                        q.qmp("query-status")
                        q.write32(RESULT, 0)
                    q.qmp("cont")
                    deadline = time.monotonic() + 5
                    while q.read32(RESULT) not in (DONE, FAULT) and time.monotonic() < deadline:
                        time.sleep(0.01)
                    q.qmp("stop")
                    values = struct.unpack("<128I", q.read(RESULT, 512))
                    self.assertEqual(values[0], FAULT if fault else DONE,
                                     f"Guest did not finish: {values}\n{q.diagnostic('MPC')}")
                    results.append(values)
            finally:
                q.close()
        return results

    def test_geometry_and_reset_lockdown_with_real_transactions(self):
        body = ""
        for index, (mpc, _) in enumerate(BANKS):
            body += f"ldr r5, ={mpc:#x}\n"
            for offset, reg in enumerate((0x10, 0x14, 0, 0x28)):
                body += f"ldr r0, [r5, #{reg}]\n" + capture(4 + index * 16 + offset * 4)
        # Use reset AUTOINC to configure adjacent LUT words with different
        # permissions. Real accesses verify the index advanced correctly.
        body += """
    ldr r5, =0x44056000
    ldr r1, [r5, #0x14]
    adds r1, #10
    movs r2, #1
    lsls r2, r2, r1
    ldr r3, =0x18000000
    adds r3, r3, r2
    ldr r0, =0x12345678
    str r0, [r3]
    ldr r0, =0xffffffff
    str r0, [r5, #0x1c]
    movs r0, #0
    str r0, [r5, #0x1c]
    ldr r0, =0x18000000
    ldr r0, [r0]
""" + capture(184) + "ldr r0, [r3]\n" + capture(188)
        body += "ldr r0, [r5, #0x18]\n" + capture(192)
        body += "movs r0, #1\nstr r0, [r5, #0x24]\n"
        # Lock the PSRAM LUT as NS, try to clear it, and issue a Secure read.
        # A real denied read proves lockdown preserved permissions.
        body += lut_word(PSRAM_MPC, 0, 0xffffffff)
        body += """
    ldr r0, =0x80000000
    str r0, [r5]
    movs r0, #0
    str r0, [r5]
    str r0, [r5, #0x1c]
    str r0, [r5, #0x28]
    ldr r0, =0x18000000
    ldr r0, [r0]
""" + capture(164) + interrupt_capture(PSRAM_MPC, 168)
        for values in self.run_guest(body, cycles=2):
            for index, (_, size) in enumerate(BANKS):
                maximum, cfg, ctrl, irq_en = values[1 + index * 4:5 + index * 4]
                self.assertEqual((maximum + 1) * 32 * (1 << (cfg + 5)), size)
                self.assertEqual((ctrl, irq_en), (0x100, 1))
            self.assertEqual(values[41], 0)
            self.assertEqual(values[42:44], (1, 0))
            self.assertEqual(values[44] & 0x30000, 0x20000)
            self.assertEqual(values[46:49], (0, 0x12345678, 2))

    def test_permissions_and_revocation_across_all_memory_aliases(self):
        cases = [("flash", FLASH_MPC, 0x280000,
                  [0x10280000, 0x30280000, 0x08280000], False),
                 ("psram", PSRAM_MPC, 0x8000,
                  [0x18008000, 0x38008000], True)]
        cases += [(f"sram{i}", 0x4405a000 + i * 0x2000, 0x8000,
                   [0x02008000 + i * 0x100000, 0x22008000 + i * 0x100000], True)
                  for i in range(8)]
        for name, mpc, offset, aliases, writable in cases:
            with self.subTest(bank=name):
                initial = 0x12345678 if writable else 0xffffffff
                updated = 0x87654321 if writable else initial
                body = ""
                if writable:
                    body += f"ldr r0, ={aliases[0]:#x}\nldr r1, ={initial:#x}\nstr r1, [r0]\n"
                body += ns_setup(aliases)
                ns = ""
                for index, alias in enumerate(aliases):
                    ns += f"ldr r3, ={alias:#x}\nldr r{index}, [r3]\n"
                if writable:
                    ns += "ldr r12, =0x87654321\nstr r12, [r3]\n"
                ns += "bx lr\n"
                body += ns_call()
                for index in range(len(aliases)):
                    body += capture(4 + 4 * index, f"r{index}")
                body += interrupt_capture(mpc, 16)
                body += sau_enable(False)
                body += f"ldr r0, ={aliases[0]:#x}\nldr r0, [r0]\n" + capture(28)
                body += lut_word(mpc, offset, 0xffffffff) + sau_enable(True) + ns_call()
                for index in range(len(aliases)):
                    body += capture(32 + 4 * index, f"r{index}")
                # SAU off makes this an actual Secure transaction to NS RAM.
                body += sau_enable(False)
                body += f"ldr r0, ={aliases[-1]:#x}\nldr r0, [r0]\n" + capture(44)
                body += interrupt_capture(mpc, 48)
                body += lut_word(mpc, offset, 0)
                body += f"ldr r0, ={aliases[0]:#x}\nldr r0, [r0]\n" + capture(60)
                body += sau_enable(True) + ns_call()
                for index in range(len(aliases)):
                    body += capture(64 + 4 * index, f"r{index}")
                body += interrupt_capture(mpc, 76) + sau_enable(False)
                body += f"ldr r0, ={aliases[0]:#x}\nldr r0, [r0]\n" + capture(88)
                values = self.run_guest(body, ns)[0]
                self.assertEqual(values[1:1 + len(aliases)], (0,) * len(aliases))
                self.assertEqual(values[4:6], (1, offset))
                self.assertEqual(values[6] & 0x30000, 0x10000)
                self.assertEqual(values[7], initial, "denied NS write must preserve bytes")
                self.assertEqual(values[8:8 + len(aliases)], (initial,) * len(aliases))
                self.assertEqual(values[11], 0, "Secure access to NS block must be denied")
                self.assertEqual(values[12:14], (1, offset))
                self.assertEqual(values[14] & 0x30000, 0x20000)
                self.assertEqual(values[15], updated)
                self.assertEqual(values[16:16 + len(aliases)], (0,) * len(aliases))
                self.assertEqual(values[19:21], (1, offset))
                self.assertEqual(values[21] & 0x30000, 0x10000)
                self.assertEqual(values[22], updated, "revoked alias must not bypass MPC")

    def test_sec_resp_generates_cpu_fault_for_denied_nonsecure_load(self):
        body = ns_setup([0x18008000])
        body += "ldr r5, =0x44056000\nmovs r0, #16\nstr r0, [r5]\n" + ns_call()
        values = self.run_guest(body, "ldr r0, =0x18008000\nldr r0, [r0]\nbx lr", fault=True)[0]
        self.assertIn(values[120], (3, 5))
        self.assertNotEqual((values[121] | values[122]) & 0x300, 0)
        self.assertEqual(values[112:114], (1, 0x8000))
        self.assertEqual(values[114] & 0x30000, 0x10000)

    def test_lut_changes_invalidate_cached_aliases_without_sau_changes(self):
        body = ("ldr r0, =0x18008000\nldr r1, =0x12345678\nstr r1, [r0]\n" +
                ns_setup([0x18008000, 0x38008000]) + ns_call() +
                capture(4) + capture(8, "r1") +
                lut_word(PSRAM_MPC, 0x8000, 0xffffffff) + ns_call() +
                capture(12) + capture(16, "r1") +
                lut_word(PSRAM_MPC, 0x8000, 0) + ns_call() +
                capture(20) + capture(24, "r1") + sau_enable(False) +
                "ldr r0, =0x18008000\nldr r0, [r0]\n" + capture(28))
        ns = """
    ldr r3, =0x18008000
    ldr r0, [r3]
    ldr r3, =0x38008000
    ldr r1, [r3]
    ldr r2, =0x87654321
    str r2, [r3]
    bx lr
"""
        values = self.run_guest(body, ns)[0]
        self.assertEqual(values[1:8],
                         (0, 0, 0x12345678, 0x12345678, 0, 0, 0x87654321))

    def test_nonsecure_configuration_writes_cannot_grant_memory_access(self):
        body = ("ldr r0, =0x18000000\nldr r1, =0x12345678\nstr r1, [r0]\n" +
                ns_setup([0x18000000], control=True) + ns_call() + capture(4) +
                interrupt_capture(PSRAM_MPC, 8) + sau_enable(False) +
                "ldr r0, =0x18000000\nldr r0, [r0]\n" + capture(20))
        ns = """
    ldr r3, =0x40056000
    movs r0, #0
    str r0, [r3]
    str r0, [r3, #0x18]
    ldr r0, =0xffffffff
    str r0, [r3, #0x1c]
    ldr r3, =0x18000000
    ldr r0, [r3]
    ldr r1, =0x87654321
    str r1, [r3]
    bx lr
"""
        values = self.run_guest(body, ns)[0]
        self.assertEqual(values[1:4], (0, 1, 0))
        self.assertEqual(values[4] & 0x30000, 0x10000)
        self.assertEqual(values[5], 0x12345678)

    def test_nsc_gateway_executes_from_secure_flash(self):
        body = ns_setup(gateway=True) + ns_call() + capture(4)
        ns = "push {r4, lr}\nldr r3, =0x100c3001\nblx r3\npop {r4, pc}\n"
        self.assertEqual(self.run_guest(body, ns)[0][1], 42)


if __name__ == "__main__":
    unittest.main()
