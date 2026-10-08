"""Check CMU command ports and execute the UART clock/opened path on M55."""

import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


SYS = 0x40000000
AON = 0x40080000
# (bank base, enable offset, disable offset, documented writable mask)
PAIRS = (
    (SYS, 0x000, 0x004, 0xFFFFFFFF),
    (SYS, 0x008, 0x00C, 0xFFFFFFFF),
    (SYS, 0x010, 0x014, 0xFFFFFFFF),
    (SYS, 0x060, 0x064, 0x3FFFFFFF),
    (SYS, 0x0E4, 0x0E8, 0x000000FF),
    (SYS, 0x100, 0x104, 0xFFFFFFFF),
    (SYS, 0x160, 0x164, 0xFFFFFFFF),
    (SYS, 0x178, 0x17C, 0xFFFFFFFF),
    (SYS, 0x244, 0x248, 0xFFFFFFFF),
    (AON, 0x004, 0x008, 0xFFFFFFFF),
    (AON, 0x040, 0x044, 0xFFFFFFFF),
    (AON, 0x0A0, 0x0A4, 0x007FFFFF),
    (AON, 0x180, 0x184, 0xFFFFFFFF),
)


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700CmuClockQtest(unittest.TestCase):
    def setUp(self):
        self.machine = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"])
        self.addCleanup(self.machine.close)

    def assert_pair(self, bank, enable, disable, expected):
        q = self.machine
        self.assertEqual(q.read32(bank + enable), expected)
        self.assertEqual(q.read32(bank + disable), expected)

    def test_enable_accumulates_disable_clears_and_zero_preserves(self):
        q = self.machine
        for bank, enable, disable, mask in PAIRS:
            with self.subTest(bank=hex(bank), enable=hex(enable)):
                self.assert_pair(bank, enable, disable, 0)
                q.write32(bank + enable, 0xA5A5A5A5)
                self.assert_pair(bank, enable, disable, 0xA5A5A5A5 & mask)
                q.write32(bank + enable, 0x5A5A5A5A)
                self.assert_pair(bank, enable, disable, mask)
                q.write32(bank + enable, 0)
                q.write32(bank + disable, 0)
                self.assert_pair(bank, enable, disable, mask)
                q.write32(bank + disable, 0xF0F0F0F0)
                self.assert_pair(bank, enable, disable, 0x0F0F0F0F & mask)
                q.write32(bank + disable, 0xFFFFFFFF)
                self.assert_pair(bank, enable, disable, 0)

    def test_command_lanes_preserve_unaddressed_bits(self):
        q = self.machine
        for bank, enable, disable, mask in PAIRS:
            with self.subTest(bank=hex(bank), enable=hex(enable)):
                # W1S with a zero byte must not erase another byte or bit.
                q.command(f"writeb {bank + enable:#x} 0x81")
                q.command(f"writeb {bank + enable + 1:#x} 0x42")
                q.command(f"writew {bank + enable + 2:#x} 0x8421")
                q.command(f"writeb {bank + enable + 1:#x} 0")
                self.assert_pair(bank, enable, disable, 0x84214281 & mask)
                q.command(f"writeb {bank + disable:#x} 0x80")
                q.command(f"writew {bank + disable + 2:#x} 0x8001")
                self.assert_pair(bank, enable, disable, 0x04204201 & mask)
                self.assertEqual(int(q.command(f"readb {bank + disable:#x}"), 0), 1)
                self.assertEqual(int(q.command(f"readw {bank + disable + 2:#x}"), 0),
                                 (0x04204201 & mask) >> 16)

    def test_groups_and_domains_are_independent_and_reset_clears_all(self):
        q = self.machine
        for index, (bank, enable, disable, mask) in enumerate(PAIRS):
            q.write32(bank + enable, index + 1)
        for index, (bank, enable, disable, mask) in enumerate(PAIRS):
            self.assert_pair(bank, enable, disable, (index + 1) & mask)
        q.write32(SYS + 0x00C, 0xFFFFFFFF)
        self.assert_pair(SYS, 0x008, 0x00C, 0)
        self.assert_pair(AON, 0x004, 0x008, 10)
        q.qmp("system_reset")
        q.qmp("query-status")
        for bank, enable, disable, mask in PAIRS:
            self.assert_pair(bank, enable, disable, 0)


UART_ASSEMBLY = r"""
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
    // The compatibility handoff enters NS state. Install its vector table
    // and enable NS configurable faults for useful test diagnostics.
    // An escalated Secure fault is instead reported by timeout/QEMU log.
    ldr r0, =vectors
    ldr r1, =0xe000ed08
    str r0, [r1]
    ldr r1, =0xe000ed24
    ldr r0, =0x00070000
    str r0, [r1]
    ldr r4, =0x20001000
    ldr r5, =0x40000000
    ldr r6, =0x4000b000
    ldr r0, =0x301
    str r0, [r6, #0x30]
    // Even an enabled UART CR must fail opened() with its PCLK disabled.
    bl uart_opened
    str r0, [r4, #4]

    ldr r0, =0x800
    str r0, [r5, #8]
    ldr r0, =0x4000
    str r0, [r5, #0x10]
    bl uart_opened
    str r0, [r4, #8]
    cbz r0, Failed
    ldr r7, =message
1:
    ldrb r0, [r7], #1
    cbz r0, 3f
2:
    ldr r1, [r6, #0x18]
    tst r1, #0x20
    bne 2b
    str r0, [r6]
    b 1b
3:
    // DISABLE is a clear command; its read port reports the new state.
    ldr r0, =0x800
    str r0, [r5, #0x0c]
    bl uart_opened
    str r0, [r4, #12]
    ldr r0, =0x800
    str r0, [r5, #8]
    bl uart_opened
    str r0, [r4, #16]
    movs r0, #3
    str r0, [r4]
4:
    wfi
    b 4b
.thumb_func
uart_opened:
    ldr r0, [r5, #0x0c]
    tst r0, #0x800
    beq 5f
    ldr r0, [r6, #0x30]
    and r0, r0, #1
    bx lr
5:
    movs r0, #0
    bx lr
.thumb_func
Failed:
    movs r0, #0xfe
    str r0, [r4]
    b 4b
.thumb_func
Fault_Handler:
    ldr r4, =0x20001000
    mrs r0, IPSR
    str r0, [r4, #20]
    movs r0, #0xff
    str r0, [r4]
    b 4b
    .ltorg
message:
    .asciz "cmu-uart-ready\n"
"""


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700CmuClockTcg(unittest.TestCase):
    def test_uart_opened_reads_clock_state_from_disable_port(self):
        commands = ("arm-none-eabi-as", "arm-none-eabi-ld", "arm-none-eabi-objcopy")
        for command in commands:
            if shutil.which(command) is None:
                self.skipTest(f"{command} is not installed")
        with tempfile.TemporaryDirectory(prefix="best1700-cmu-program-") as temp:
            directory = Path(temp)
            source = directory / "uart.s"
            obj = directory / "uart.o"
            elf = directory / "uart.elf"
            binary = directory / "uart.bin"
            source.write_text(UART_ASSEMBLY)
            for command in (
                [commands[0], "-march=armv8-m.main", "-mthumb", "-o", str(obj), str(source)],
                [commands[1], "-Ttext=0x101c0000", "-e", "0x101c0401",
                 "-o", str(elf), str(obj)],
                [commands[2], "-O", "binary", str(elf), str(binary)],
            ):
                completed = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(completed.returncode, 0, completed.stderr)
            q = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"],
                             image_payload=binary.read_bytes(), accel="tcg",
                             machine_options=",boot-pc=0x101c0401,boot-sp=0x203ff000",
                             extra_args=("-S", "-icount", "shift=0,sleep=off"))
            try:
                q.qmp("cont")
                result = (0,) * 6
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    result = struct.unpack("<6I", q.read(0x20001000, 24))
                    if result[0] in (3, 0xFE, 0xFF):
                        break
                    time.sleep(0.01)
                q.qmp("stop")
                self.assertEqual(result, (3, 0, 1, 0, 1, 0),
                                 f"guest state={result}\n{q.diagnostic('UART opened')}")
                self.assertEqual(q.serial(0), b"cmu-uart-ready\n")
            finally:
                q.close()


if __name__ == "__main__":
    unittest.main()
