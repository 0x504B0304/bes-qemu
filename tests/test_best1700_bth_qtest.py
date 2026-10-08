"""Exercise BTH SRAM ownership, cache control and guest-driven reset vectors.

All code is synthetic. Tests own their QEMU processes and never attach to a
running firmware session. Unselected RAM response is explicitly a diagnostic
RAZ/WI fallback, not an asserted hardware fault contract.
"""

import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


CMU = 0x40080000
RAM_CFG = CMU + 0x98
VTOR = CMU + 0xB8
RESET_PULSE = CMU + 0xC4
RESET_SET = CMU + 0xC8
RESET_CLEAR = CMU + 0xCC
RESULT = 0x23001000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BthTests(unittest.TestCase):
    def machine(self, *, real=True, btc=False, tcg=False, image=None):
        options = ",boot-sp=0x203ff000,boot-pc=0x101c0001"
        if real:
            options += ",bth-real-boot=on"
        if btc:
            options += ",btc-real-boot=on"
        args = ["-S", "-icount", "shift=3,sleep=off"] if tcg else []
        if btc:
            args += ["-smp", "4"]
        machine = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=image or (b"\xfe\xe7" + bytes(32)), machine_options=options,
            accel="tcg" if tcg else "qtest",
            extra_args=args)
        self.addCleanup(machine.close)
        return machine

    @staticmethod
    def status(machine, name="bth-status"):
        return json.loads(machine.qmp("qom-get", {
            "path": "/machine", "property": name}))

    def test_default_mode_keeps_legacy_independent_ram(self):
        machine = self.machine(real=False)
        self.assertEqual(self.status(machine)["mode"], "stub")
        self.assertEqual(self.status(machine, "core-status")["bth"], "stub")
        machine.write32(0x230C0000, 0x11223344)
        machine.write32(0x22780000, 0x55667788)
        self.assertEqual(machine.read32(0x230C0000), 0x11223344)
        self.assertEqual(machine.read32(0x22780000), 0x55667788)

    def test_shared_blocks_retain_contents_when_owner_changes(self):
        machine = self.machine()
        self.assertEqual(machine.read32(RAM_CFG) & 0x03FFFFFF, 0)
        machine.write32(0x23000400, 0xC0DEC0DE)
        self.assertEqual(machine.read32(0x03000400), 0xC0DEC0DE)
        # Physical bank indices increase in SYS and decrease in BTH.
        for bank in range(13):
            bth = 0x230C0000 + (12 - bank) * 0x20000
            machine.write32(bth + 0x104, 0xB0000000 | bank)
        machine.write32(RAM_CFG, 0x03FFFFFF)  # all controlled banks to SYS
        for bank in range(13):
            sysram = 0x02600000 + bank * 0x20000
            self.assertEqual(machine.read32(sysram + 0x104), 0xB0000000 | bank)
            self.assertEqual(machine.read32(sysram + 0x20000104),
                             0xB0000000 | bank)
            machine.write32(sysram + 0x108, 0xA0000000 | bank)
        # Unsupported BTH access is counted and cannot mutate SYS storage.
        count = self.status(machine)["unsupported_accesses"]
        self.assertEqual(machine.read32(0x230C0104), 0)
        machine.write32(0x230C0104, 0xFFFFFFFF)
        self.assertEqual(machine.read32(0x02780104), 0xB000000C)
        self.assertEqual(self.status(machine)["unsupported_accesses"], count + 2)
        machine.write32(RAM_CFG, 0)
        for bank in range(13):
            bth = 0x230C0000 + (12 - bank) * 0x20000
            self.assertEqual(machine.read32(bth + 0x108), 0xA0000000 | bank)
            self.assertEqual(machine.read32(bth - 0x20000000 + 0x104),
                             0xB0000000 | bank)
        # The three blocks outside the 13 owner fields remain SYS memory.
        machine.write32(0x027A0000, 0xCAFEBABE)
        machine.write32(RAM_CFG, 0x01555555)  # BTC owns these banks.
        self.assertEqual(machine.read32(0x027A0000), 0xCAFEBABE)
        self.assertEqual(machine.read32(0x23000400), 0xC0DEC0DE)
        self.assertEqual(machine.read32(0x02600104), 0)
        self.assertEqual(machine.read32(0x23240104), 0)

    def test_btc_shared_banks_keep_sys_order_and_one_physical_copy(self):
        machine = self.machine(btc=True)
        for bank in range(13):
            bth = 0x230C0000 + (12 - bank) * 0x20000
            # Include the last word so the mapping is checked at each edge.
            for offset in (0, 0x1FFFC):
                machine.write32(bth + offset,
                                0xB0000000 | (bank << 20) | offset)
        machine.write32(RAM_CFG, 0x01555555)
        denied_before = self.status(machine)["unsupported_accesses"]
        for bank in range(13):
            btc = 0x68400000 + bank * 0x20000
            bth = 0x230C0000 + (12 - bank) * 0x20000
            sysram = 0x02600000 + bank * 0x20000
            for offset in (0, 0x1FFFC):
                expected = 0xB0000000 | (bank << 20) | offset
                self.assertEqual(machine.read32(btc + offset), expected)
                self.assertEqual(machine.read32(btc - 0x08000000 + offset),
                                 expected)
            self.assertEqual(machine.read32(sysram), 0)
            self.assertEqual(machine.read32(bth), 0)
            machine.write32(sysram, 0xBAD00000)
            machine.write32(bth, 0xBAD00001)
            self.assertEqual(machine.read32(btc), 0xB0000000 | (bank << 20))
            machine.write32(btc + 4, 0xC0000000 | bank)
            machine.write32(btc - 0x08000000 + 8, 0xC1000000 | bank)
        self.assertEqual(self.status(machine)["unsupported_accesses"],
                         denied_before + 13 * 4)
        # WIFI ownership cannot expose or overwrite any of these apertures.
        machine.write32(RAM_CFG, 0x02AAAAAA)
        for bank in range(13):
            btc = 0x68400000 + bank * 0x20000
            self.assertEqual(machine.read32(btc), 0)
            machine.write32(btc + 4, 0xBAD00002)
        machine.write32(RAM_CFG, 0x03FFFFFF)
        for bank in range(13):
            sysram = 0x02600000 + bank * 0x20000
            self.assertEqual(machine.read32(sysram + 4), 0xC0000000 | bank)
            self.assertEqual(machine.read32(sysram + 8), 0xC1000000 | bank)
            self.assertEqual(machine.read32(0x60400000 + bank * 0x20000), 0)
        machine.write32(RAM_CFG, 0)
        for bank in range(13):
            bth = 0x230C0000 + (12 - bank) * 0x20000
            self.assertEqual(machine.read32(bth + 4), 0xC0000000 | bank)
            self.assertEqual(machine.read32(bth + 8), 0xC1000000 | bank)

    def test_btc_owner_byte_lanes_and_reset_preserve_shared_storage(self):
        machine = self.machine(btc=True)
        machine.write32(0x68001000, 0x11223344)
        machine.write32(0x027A0000, 0x55667788)
        machine.command(f"writeb {RAM_CFG:#x} 1")
        machine.command(f"writeb {RAM_CFG + 3:#x} 1")
        self.assertEqual(machine.read32(RAM_CFG), 0x01000001)
        machine.write32(0x68400000, 0xC0010000)
        machine.write32(0x6859FFFC, 0xC001000C)
        self.assertEqual(machine.read32(0x60400000), 0xC0010000)
        self.assertEqual(machine.read32(0x6059FFFC), 0xC001000C)
        self.assertEqual(machine.read32(0x23240000), 0)
        self.assertEqual(machine.read32(0x230DFFFC), 0)
        # Adjacent banks remain owned by BTH after byte-lane writes.
        machine.write32(0x23220000, 0xB0010001)
        self.assertEqual(machine.read32(0x68420000), 0)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        self.assertEqual(machine.read32(RAM_CFG) & 0x03FFFFFF, 0)
        self.assertEqual(machine.read32(0x68400000), 0)
        self.assertEqual(machine.read32(0x60400000), 0)
        self.assertEqual(machine.read32(0x23240000), 0xC0010000)
        self.assertEqual(machine.read32(0x230DFFFC), 0xC001000C)
        self.assertEqual(machine.read32(0x23220000), 0xB0010001)
        self.assertEqual(machine.read32(0x68001000), 0x11223344)
        self.assertEqual(machine.read32(0x027A0000), 0x55667788)

    def test_ram_owner_byte_lanes_and_reset_status(self):
        machine = self.machine()
        machine.write32(RAM_CFG, 0x03FFFFFF)
        machine.command(f"writeb {RAM_CFG + 2:#x} 0")
        machine.command(f"writeb {RAM_CFG + 3:#x} 0")
        self.assertEqual(machine.read32(RAM_CFG), 0xFFFF)
        # Both status views report 1 for a released reset line.
        self.assertEqual(machine.read32(RESET_SET) & 0x1300, 0)
        self.assertEqual(machine.read32(RESET_CLEAR) & 0x1300, 0)
        machine.write32(RESET_CLEAR, 0x100)
        self.assertEqual(machine.read32(RESET_SET) & 0x1300, 0x100)
        self.assertEqual(machine.read32(RESET_CLEAR) & 0x1300, 0x100)
        self.assertEqual(self.status(machine, "core-status")["bth"], "held-reset")
        machine.write32(RESET_SET, 0x100)
        self.assertEqual(machine.read32(RESET_CLEAR) & 0x1300, 0)

    def test_cache_register_lanes_commands_and_reset(self):
        machine = self.machine()
        machine.write32(0x23006000, 0x11223344)
        for base in (0x0338A000, 0x0348A000):
            for offset in (0, 8, 0x1C):
                address = base + offset
                self.assertEqual(machine.read32(address), 0)
                machine.write32(address, 0x89ABCDEF)
                machine.command(f"writeb {address + 1:#x} 0x12")
                machine.command(f"writew {address + 2:#x} 0x3456")
                self.assertEqual(machine.read32(address), 0x345612EF)
                self.assertEqual(int(machine.command(f"readb {address + 3:#x}"), 0),
                                 0x34)
                self.assertEqual(int(machine.command(f"readw {address:#x}"), 0),
                                 0x12EF)
            # Commands complete synchronously. They do not behave like RAM
            # and must not alter the bytes whose cache lines they maintain.
            for offset, value in ((4, 1), (0x18, 0x03006013), (0x38, 1)):
                machine.write32(base + offset, value)
                self.assertEqual(machine.read32(base + offset), 0)
            for offset in (0, 8, 0x1C):
                self.assertEqual(machine.read32(base + offset), 0x345612EF)
            self.assertEqual(machine.read32(0x23006000), 0x11223344)
            self.assertEqual(machine.read32(0x03006000), 0x11223344)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        for base in (0x0338A000, 0x0348A000):
            for offset in (0, 8, 0x1C):
                self.assertEqual(machine.read32(base + offset), 0)

    def test_bth_fast_counter_frequency_lanes_and_reset(self):
        machine = self.machine()
        clock = 0x5000005C
        low, high = 0x50000090, 0x50000094
        self.assertEqual(machine.read32(low), 0)
        machine.step(1_000_000)
        self.assertEqual(machine.read32(low), 6000)
        machine.command(f"writeb {clock + 1:#x} 0xf")
        machine.command(f"writeb {clock:#x} 0x40")
        self.assertEqual(machine.read32(clock), 0xF40)
        machine.step(1_000_000)
        self.assertEqual(machine.read32(low), 9000)
        machine.command(f"writeb {clock:#x} 0x80")
        machine.step(1_000_000)
        self.assertEqual(machine.read32(low), 10500)
        # Counter status is read-only and its high word advances on wrap.
        machine.write32(low, 1)
        self.assertEqual(machine.read32(low), 10500)
        machine.write32(clock, 0xF00)
        machine.step(1_000_000_000_000)
        ticks = 6_000_010_500
        self.assertEqual(machine.read32(low), ticks & 0xFFFFFFFF)
        self.assertEqual(machine.read32(high), ticks >> 32)
        # The CMU container is sparse: unrelated state remains accessible.
        machine.write32(0x500000A8, 0x12345678)
        self.assertEqual(machine.read32(0x500000A8), 0x12345678)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        self.assertEqual(machine.read32(low), 0)
        self.assertEqual(machine.read32(high), 0)
        self.assertEqual(machine.read32(clock), 0)

    def test_bth_fast_counter_set_registers_reseed_running_counter(self):
        machine = self.machine()
        low, high = 0x50000090, 0x50000094
        low_set, high_set = 0x50000098, 0x5000009C
        machine.write32(low_set, 0x12345678)
        self.assertEqual(machine.read32(low), 0x12345678)
        machine.write32(high_set, 0x9ABCDEF0)
        self.assertEqual(machine.read32(low), 0x12345678)
        self.assertEqual(machine.read32(high), 0x9ABCDEF0)
        machine.step(1_000_000)
        ticks = (0x9ABCDEF0 << 32) | 0x12345678
        ticks += 6000
        self.assertEqual(machine.read32(low), ticks & 0xFFFFFFFF)
        self.assertEqual(machine.read32(high), ticks >> 32)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        self.assertEqual(machine.read32(low), 0)
        self.assertEqual(machine.read32(high), 0)

    def test_bth_timer_interrupts_and_clock_selection(self):
        machine = self.machine()
        machine.command("irq_intercept_in /machine/core[2]")
        machine.write32(0x5000005C, 0x300)  # BTH timers 0 and 1 use 6 MHz.
        for base, irq in ((0x4008B000, 19), (0x50002000, 23), (0x50003000, 25)):
            for sub in range(2):
                timer = base + sub * 0x20
                machine.write32(timer, 15)
                machine.write32(timer + 8, 0xA3)  # IRQ, 32-bit, one-shot
                machine.step(1_000_000)
                self.assertEqual(machine.read32(timer + 0x10), 1)
                self.assertTrue(machine.irq(irq + sub))
                for other in (19, 20, 23, 24, 25, 26, 66, 67):
                    if other != irq + sub:
                        self.assertFalse(machine.irq(other), f"unexpected IRQ{other}")
                machine.write32(timer + 0xC, 1)
                self.assertFalse(machine.irq(irq + sub))
        # Both subtimers of a BTH timer follow that timer's source selection.
        machine.write32(0x5000005C, 0)
        machine.write32(0x50002000, 5999)
        machine.write32(0x50002008, 0xA3)
        machine.step(1_000_000)
        self.assertFalse(machine.irq(23))
        machine.write32(0x5000005C, 0x100)
        machine.step(1_000_000)
        self.assertTrue(machine.irq(23))
        machine.qmp("system_reset")
        machine.qmp("query-status")
        for irq in (19, 20, 23, 24, 25, 26):
            self.assertFalse(machine.irq(irq))

    def assemble(self, source=None, base=0x03004000):
        names = ["arm-none-eabi-as", "arm-none-eabi-ld", "arm-none-eabi-objcopy"]
        commands = [shutil.which(name) for name in names]
        if not all(commands):
            self.skipTest("GNU Arm binutils are not installed")
        source = source or r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    movs r5, #0xa1
    b common
.org 0x100
.thumb_func
reset_b:
    movs r5, #0xb2
common:
    ldr r4, =0x23001000
    mrs r0, msp
    str r0, [r4, #4]
    ldr r0, =0xe000ed08
    ldr r0, [r0]
    str r0, [r4, #8]
    ldr r0, =0x400800b8
    ldr r0, [r0]
    str r0, [r4, #12]
    ldr r0, =0x23000600
    ldr r0, [r0]
    str r0, [r4, #16]
    str r5, [r4]
1:  wfi
    b 1b
    .ltorg
"""
        with tempfile.TemporaryDirectory(prefix="best1700-bth-program-") as temp:
            directory = Path(temp)
            asm = directory / "bth.s"
            obj = directory / "bth.o"
            elf = directory / "bth.elf"
            binary = directory / "bth.bin"
            asm.write_text(source)
            for command in (
                [commands[0], "-mthumb", "-o", str(obj), str(asm)],
                [commands[1], f"-Ttext={base:#x}", "-e", "reset_a",
                 "-o", str(elf), str(obj)],
                [commands[2], "-O", "binary", str(elf), str(binary)],
            ):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            return binary.read_bytes()

    def await_marker(self, machine, value):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if machine.read32(RESULT) == value:
                return
            time.sleep(0.01)
        self.fail(machine.diagnostic(f"BTH did not produce marker {value:#x}"))

    def test_guest_vectors_release_restart_and_system_reset(self):
        code = self.assemble()
        machine = self.machine(tcg=True)
        machine.write(0x23004000, code)
        machine.write32(0x23000600, 0x12345678)
        machine.write(0x23002000, struct.pack("<II", 0x230BFF00, 0x03004001))
        machine.write(0x23002100, struct.pack("<II", 0x230BEE00, 0x03004101))
        machine.write32(VTOR, 0x23002000)
        machine.qmp("cont")
        # Subsystem release alone leaves CPU0 held and the guest untouched.
        machine.write32(RESET_CLEAR, 0x100)
        self.assertEqual(self.status(machine, "core-status")["bth"], "held-reset")
        self.assertEqual(machine.read32(RESULT), 0)

        machine.write32(RESET_CLEAR, 0x200)
        self.await_marker(machine, 0xA1)
        machine.qmp("stop")
        self.assertEqual(struct.unpack("<5I", machine.read(RESULT, 20)),
                         (0xA1, 0x230BFF00, 0x23002000, 0x23002000, 0x12345678))
        self.assertEqual(self.status(machine, "core-status")["bth"], "running")
        # No image/profile address can substitute for this changed vector.
        machine.write32(RESET_SET, 0x200)
        machine.write32(VTOR, 0x23002100)
        machine.write32(RESULT, 0)
        machine.write32(RESET_CLEAR, 0x200)
        machine.qmp("cont")
        self.await_marker(machine, 0xB2)
        machine.qmp("stop")
        self.assertEqual(machine.read32(RESULT + 4), 0x230BEE00)
        self.assertEqual(machine.read32(RESULT + 8), 0x23002100)
        # Pulse resets the target context without changing release latches.
        machine.write32(RESULT, 0)
        machine.write32(RESET_PULSE, 0x200)
        machine.qmp("cont")
        self.await_marker(machine, 0xB2)
        machine.qmp("stop")
        self.assertEqual(machine.read32(RESET_SET) & 0x300, 0x300)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        machine.write32(RESULT, 0)
        machine.qmp("cont")
        self.assertEqual(self.status(machine, "core-status")["bth"], "held-reset")
        self.assertEqual(machine.read32(VTOR) & ~127, 0)
        self.assertEqual(machine.read32(RESULT), 0)

    def test_ap_guest_instruction_releases_bth_under_icount(self):
        ap = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x23002000
    ldr r0, =0x230bff00
    str r0, [r4]
    ldr r0, =0x03004001
    str r0, [r4, #4]
    ldr r5, =0x40080000
    str r4, [r5, #0xb8]
    mov.w r0, #0x100
    str r0, [r5, #0xcc]
    dsb sy
    mov.w r0, #0x200
    str r0, [r5, #0xcc]
    ldr r4, =0x23001000
1:  ldr r0, [r4]
    cmp r0, #0xa1
    bne 1b
    movs r0, #0xac
    str r0, [r4, #24]
2:  wfi
    b 2b
    .ltorg
""", base=0x101C0000)
        machine = self.machine(tcg=True, image=ap)
        machine.write(0x23004000, self.assemble())
        machine.qmp("cont")
        self.await_marker(machine, 0xA1)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and machine.read32(RESULT + 24) != 0xAC:
            time.sleep(0.01)
        machine.qmp("stop")
        self.assertEqual(machine.read32(RESULT + 24), 0xAC,
                         "AP must resume after releasing its peer")
        self.assertEqual(machine.read32(RESULT + 4), 0x230BFF00)

    def test_bth_guest_cache_enable_and_modified_instruction_visibility(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x23001000
    ldr r5, =0x0338a000
    ldr r0, [r5]
    str r0, [r4, #4]
    movs r0, #1
    str r0, [r5, #4]
    str r0, [r5]
    ldr r0, [r5]
    str r0, [r4, #8]
    ldr r6, =0x03006001
    blx r6
    str r0, [r4, #12]
    ldr r7, =0x23006000
    ldr r0, =0x47702022
    str r0, [r7]
    str r6, [r5, #0x18]
    dsb sy
    isb sy
    blx r6
    str r0, [r4, #16]
    movs r0, #1
    str r0, [r5, #0x38]
    ldr r0, [r5, #0x38]
    str r0, [r4, #20]
    movs r0, #0xc3
    str r0, [r4]
1:  wfi
    b 1b
    .ltorg
""")
        machine = self.machine(tcg=True)
        machine.write(0x23004000, code)
        machine.write32(0x23006000, 0x47702011)  # movs r0, #0x11; bx lr
        machine.write(0x23002000, struct.pack("<II", 0x230BFF00, 0x03004001))
        machine.write32(VTOR, 0x23002000)
        machine.qmp("cont")
        machine.write32(RESET_CLEAR, 0x300)
        self.await_marker(machine, 0xC3)
        machine.qmp("stop")
        self.assertEqual(struct.unpack("<6I", machine.read(RESULT, 24)),
                         (0xC3, 0, 1, 0x11, 0x22, 0))

    def test_bth_guest_fast_delay_then_aon_timer_interrupt(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x23001000
    ldr r5, =0x50000000
    ldr r0, [r5, #0x5c]
    str r0, [r4, #4]
    ldr r6, [r5, #0x90]
1:  ldr r0, [r5, #0x90]
    subs r0, r0, r6
    cmp r0, #12
    blo 1b
    str r0, [r4, #8]
    ldr r5, =0xe000e100
    mov.w r0, #0x80000
    str r0, [r5]
    ldr r5, =0x4008b000
    movs r0, #15
    str r0, [r5]
    movs r0, #0xa3
    str r0, [r5, #8]
    cpsie i
2:  wfi
    b 2b
    .ltorg
.org 0x100
.thumb_func
timer_irq:
    ldr r1, =0x4008b000
    movs r0, #1
    str r0, [r1, #12]
    ldr r1, =0x23001000
    movs r0, #0xd4
    str r0, [r1]
    bx lr
    .ltorg
""")
        machine = self.machine(tcg=True)
        machine.write(0x23004000, code)
        vectors = bytearray((16 + 20) * 4)
        struct.pack_into("<II", vectors, 0, 0x230BFF00, 0x03004001)
        struct.pack_into("<I", vectors, (16 + 19) * 4, 0x03004101)
        machine.write(0x23002000, vectors)
        machine.write32(0x5000005C, 0xF00)
        machine.write32(VTOR, 0x23002000)
        machine.qmp("cont")
        machine.write32(RESET_CLEAR, 0x300)
        self.await_marker(machine, 0xD4)
        machine.qmp("stop")
        self.assertEqual(machine.read32(RESULT + 4), 0xF00,
                         "AP and BTH must share the timing registers")
        self.assertGreaterEqual(machine.read32(RESULT + 8), 12)



if __name__ == "__main__":
    unittest.main()
