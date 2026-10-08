"""Exercise BTC boot flags and reject unimplemented power transitions."""

import os
import struct
import time
import unittest

from tests import test_best1700_bth_qtest as bth_qtest
from tests.test_best1700_peripherals_qtest import QtestMachine


BTC_AON = 0x6E060000
AON = 0x40080000
RESULT = 0x68001000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcAonTests(unittest.TestCase):
    assemble = bth_qtest.BthTests.assemble

    def machine(self, *, tcg=False):
        args = ["-d", "unimp", "-smp", "4"]
        if tcg:
            args += ["-S", "-icount", "shift=3,sleep=off"]
        machine = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=b"\xfe\xe7" + bytes(32),
            machine_options=",bth-real-boot=on,btc-real-boot=on,boot-sp=0x203ff000,boot-pc=0x101c0001",
            accel="tcg" if tcg else "qtest", extra_args=args)
        self.addCleanup(machine.close)
        return machine

    def test_guest_flags_survive_cpu_reset_but_machine_reset_clears_them(self):
        machine = self.machine()
        self.assertEqual(machine.read32(BTC_AON), 0)
        self.assertEqual(machine.read32(BTC_AON + 4), 0)
        for value in (1, 5, 0x12345678, 0xFFFFFFFF):
            machine.write32(BTC_AON, value)
            self.assertEqual(machine.read32(BTC_AON), value)
            self.assertEqual(machine.read32(BTC_AON + 4), 0)
        machine.write32(AON + 0xCC, 0x80)
        machine.write32(AON + 0xC8, 0x80)
        self.assertEqual(machine.read32(BTC_AON), 0xFFFFFFFF)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        self.assertEqual(machine.read32(BTC_AON), 0)
        self.assertEqual(machine.read32(BTC_AON + 4), 0)

    def test_unsupported_power_requests_and_registers_do_not_change_state(self):
        machine = self.machine()
        machine.write32(BTC_AON, 5)
        for request in (0, 0x20, 0x40, 0x80000000, 0xFFFFFFFF):
            machine.write32(BTC_AON + 4, request)
            self.assertEqual(machine.read32(BTC_AON + 4), 0)
        for offset in (8, 0x100, 0xFFC):
            machine.write32(BTC_AON + offset, 0xFFFFFFFF)
            self.assertEqual(machine.read32(BTC_AON + offset), 0)
        self.assertEqual(machine.read32(BTC_AON), 5)
        log = machine.stderr_path.read_text(errors="replace")
        for value in ("0x0", "0x20", "0x40", "0x80000000", "0xffffffff"):
            self.assertIn(f"BTC AON unsupported write offset=0x4 value={value}\n", log)
        for offset in ("0x8", "0x100", "0xffc"):
            self.assertIn(f"BTC AON unsupported write offset={offset}", log)
            self.assertIn(f"BTC AON unsupported read offset={offset}", log)

    def test_short_and_unaligned_writes_do_not_modify_boot_flags(self):
        machine = self.machine()
        machine.write32(BTC_AON, 0x12345678)
        machine.command(f"writeb {BTC_AON:#x} 0xff")
        machine.command(f"writew {BTC_AON:#x} 0xffff")
        machine.command(f"writel {BTC_AON + 1:#x} 0xffffffff")
        self.assertEqual(machine.read32(BTC_AON), 0x12345678)

    def test_btc_reads_cold_state_and_unsupported_power_write_faults(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset
.thumb_func
reset:
    ldr r4, =0x68001000
    ldr r5, =0x6e060000
    ldr r0, [r5, #4]
    str r0, [r4]
    ldr r0, [r5]
    str r0, [r4, #4]
    orr r0, r0, #1
    str r0, [r5]
    ldr r0, [r5]
    str r0, [r4, #8]
    movs r0, #0x20
    str r0, [r5, #4]
    str r0, [r4, #0x18]
1:  wfi
    b 1b
    .ltorg
    .org 0x200
.thumb_func
hardfault:
    ldr r4, =0x68001000
    ldr r5, =0xe000ed00
    ldr r0, [r5, #0x38]
    str r0, [r4, #0x10]
    ldr r0, [r5, #0x28]
    str r0, [r4, #0x14]
    movw r0, #0xbad
    str r0, [r4, #0xc]
2:  wfi
    b 2b
    .ltorg
""", base=0x68004000)
        machine = self.machine(tcg=True)
        machine.write(0x68004000, code)
        machine.write(0x68002000, struct.pack(
            "<4I", 0x68017F00, 0x68004001, 0, 0x68004201))
        machine.write(RESULT, bytes([0xFF]) * 0x1C)
        machine.write32(BTC_AON, 4)
        machine.write32(AON + 0x168, 0x68002000)
        machine.qmp("cont")
        machine.write32(AON + 0xCC, 0x88)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if machine.read32(RESULT + 0xC) == 0xBAD:
                break
            time.sleep(0.01)
        else:
            self.fail(machine.diagnostic("BTC did not report the unsupported power write"))
        machine.qmp("stop")
        self.assertEqual(struct.unpack("<3I", machine.read(RESULT, 12)), (0, 4, 5))
        self.assertEqual(machine.read32(BTC_AON), 5)
        self.assertEqual(machine.read32(BTC_AON + 4), 0)
        self.assertEqual(machine.read32(RESULT + 0x10), BTC_AON + 4)
        self.assertEqual(machine.read32(RESULT + 0x14) & 0x8200, 0x8200)
        self.assertEqual(machine.read32(RESULT + 0x18), 0xFFFFFFFF)
        self.assertNotIn("Lockup", machine.stderr_path.read_text(errors="replace"))


if __name__ == "__main__":
    unittest.main()
