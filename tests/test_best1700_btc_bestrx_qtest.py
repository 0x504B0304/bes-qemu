"""Verify BESTRX configuration retention without a packet or event engine."""

import os
import struct
import time
import unittest

from tests import test_best1700_bth_qtest as bth_qtest
from tests.test_best1700_peripherals_qtest import QtestMachine


BASEBAND = 0x6D040000
AON = 0x40080000
RESULT = 0x68001000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcBestrxTests(unittest.TestCase):
    assemble = bth_qtest.BthTests.assemble

    def machine(self, *, tcg=False):
        args = ["-smp", "4", "-d", "unimp"]
        if tcg:
            args += ["-S", "-icount", "shift=3,sleep=off"]
        q = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=b"\xfe\xe7" + bytes(32),
            machine_options=(",cp-real-boot=on,bth-real-boot=on,btc-real-boot=on"
                             ",boot-sp=0x203ff000,boot-pc=0x101c0001"),
            accel="tcg" if tcg else "qtest", extra_args=args)
        self.addCleanup(q.close)
        return q

    def test_original_and_variant_cold_values_do_not_change_em_or_raise_irq(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0x80FF)
        q.write32(BASEBAND + 0x40C, 0x10017)
        q.write32(BASEBAND + 0x80C, 0x101FF)
        self.assertEqual(q.read32(BASEBAND + 0xE68), 0)
        self.assertEqual(q.read32(BASEBAND + 0xE6C), 0)
        # Both control structures, original descriptors and an event target.
        memory = {0x6D02D838: 0xA5120010, 0x6D02D858: 0x12341B0C,
                  0x6D02D880: 0x11223344, 0x6D03A098: 0x5A340011,
                  0x68006000: 0x89ABCDEF}
        for address, value in memory.items():
            q.write32(address, value)
        q.write32(BASEBAND + 0xCA4, 0x68006000)
        q.write32(BASEBAND + 0xCAC, 0xAAAAAAAA)
        q.write32(BASEBAND + 0xC04, 0x03000000)
        for control in (0xF78C5B07, 0xF7847413):
            q.write32(BASEBAND + 0xE6C, control)
            q.write32(BASEBAND + 0xE68, 0x323C328C)
            q.command("clock_step 100000000")
            self.assertEqual(q.read32(BASEBAND + 0xE6C), control)
            self.assertEqual(q.read32(BASEBAND + 0xE68), 0x323C328C)
            for address, value in memory.items():
                self.assertEqual(q.read32(address), value)
            for offset in (0x1C, 0x410, 0x460, 0x810, 0x860):
                self.assertEqual(q.read32(BASEBAND + offset), 0)
            for irq in (0, 1, 2, 29):
                self.assertFalse(q.irq(irq))
        self.assertNotIn("baseband unsupported",
                         q.stderr_path.read_text(errors="replace"))

    def test_vendor_rmw_preserves_other_parameters_and_accepts_non_cold_values(self):
        q = self.machine()
        q.write32(BASEBAND + 0xE68, 0x323C328C)
        for clear, value, expected in ((0xFF000000, 0x12000000, 0x123C328C),
                                        (0x00FF0000, 0x00340000, 0x1234328C),
                                        (0x0000FF00, 0x00005600, 0x1234568C)):
            q.write32(BASEBAND + 0xE68,
                      (q.read32(BASEBAND + 0xE68) & ~clear) | value)
            self.assertEqual(q.read32(BASEBAND + 0xE68), expected)
        q.write32(BASEBAND + 0xE6C, 0xF78C5B07)
        q.write32(BASEBAND + 0xE6C,
                  (q.read32(BASEBAND + 0xE6C) & ~0xF0000000) | 0xA0000000)
        self.assertEqual(q.read32(BASEBAND + 0xE6C), 0xA78C5B07)
        q.write32(BASEBAND + 0xE6C, q.read32(BASEBAND + 0xE6C) | 0x20000)
        self.assertEqual(q.read32(BASEBAND + 0xE6C), 0xA78E5B07)
        q.write32(BASEBAND + 0xE6C, q.read32(BASEBAND + 0xE6C) & ~0x20000)
        self.assertEqual(q.read32(BASEBAND + 0xE6C), 0xA78C5B07)
        # Opaque-word compatibility must not become a cold-constant allowlist.
        for offset in (0xE68, 0xE6C):
            for value in (0, 0xFFFFFFFF, 0x81B3579D):
                q.write32(BASEBAND + offset, value)
                self.assertEqual(q.read32(BASEBAND + offset), value)

    def test_btc_guest_getter_modes_and_reset_retention(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x68005000
    ldr r5, =0x68001000
    ldr r7, =0x6d040000
    movs r6, #4
1:  ldr r0, [r4], #4
    ldr r1, [r4], #4
    str r0, [r7, #0xe68]
    str r1, [r7, #0xe6c]
    bl getter
    str r0, [r5], #4
    subs r6, r6, #1
    bne 1b
    movs r0, #1
    str r0, [r5]
2:  wfi
    b 2b
.thumb_func
getter:
    ldr r3, =0x6d040000
    ldr r0, [r3, #0xe68]
    ldr r2, [r3, #0xe6c]
    uxtb r0, r0
    tst r2, #0xc000
    beq 4f
    ldr r3, [r3, #0xe6c]
    ubfx r3, r3, #14, #2
    cmp r3, #1
    bne 3f
    lsrs r0, r0, #1
    bx lr
3:  movs r0, #0
4:  bx lr
    .ltorg
""", base=0x68004000)
        q = self.machine(tcg=True)
        # Preserve unrelated high fields while selecting all four mode values.
        values = (0x323C328C, 0xF78C1B07, 0xA1B2C3FF, 0xF78C5B07,
                  0x11223377, 0xF78C9B07, 0xAABBCCDD, 0xF78CDB07)
        q.write(0x68005000, struct.pack("<8I", *values))
        q.write(0x68004000, code)
        q.write(0x68002000, struct.pack("<II", 0x68017F00, 0x68004001))
        q.write32(AON + 0x168, 0x68002000)
        q.qmp("cont")
        q.write32(AON + 0xCC, 0x88)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and q.read32(RESULT + 16) != 1:
            time.sleep(0.01)
        q.qmp("stop")
        self.assertEqual(q.read32(RESULT + 16), 1,
                         q.diagnostic("BTC did not finish BESTRX getter"))
        self.assertEqual(tuple(q.read32(RESULT + 4 * i) for i in range(4)),
                         (140, 127, 0, 0))
        q.write32(AON + 0xC8, 0x80)
        for offset, value in ((0xE68, values[-2]), (0xE6C, values[-1])):
            self.assertEqual(q.read32(BASEBAND + offset), value)
        q.write32(BASEBAND, 0x80000000)
        self.assertEqual(q.read32(BASEBAND + 0xE68), values[-2])
        self.assertEqual(q.read32(BASEBAND + 0xE6C), values[-1])
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertEqual(q.read32(BASEBAND + 0xE68), 0)
        self.assertEqual(q.read32(BASEBAND + 0xE6C), 0)

    def test_neighboring_interrupt_commands_remain_unsupported(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0xE68, 0x12345678)
        q.write32(BASEBAND + 0xE6C, 0xF13579BD)
        for value in (1, 4, 0xFFFFFFFF):
            q.write32(BASEBAND + 0xE64, value)
        self.assertEqual(q.read32(BASEBAND + 0xE64), 0)
        self.assertEqual(q.read32(BASEBAND + 0xE68), 0x12345678)
        self.assertEqual(q.read32(BASEBAND + 0xE6C), 0xF13579BD)
        q.command("clock_step 100000000")
        for irq in (0, 1, 2, 29):
            self.assertFalse(q.irq(irq))
        log = q.stderr_path.read_text(errors="replace")
        self.assertIn("unsupported write offset=0xe64", log)
        self.assertIn("unsupported read offset=0xe64", log)


if __name__ == "__main__":
    unittest.main()
