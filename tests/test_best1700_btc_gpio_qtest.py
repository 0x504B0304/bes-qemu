"""Exercise shared BTC GPIO/test-port configuration without pin or IRQ events."""

import os
import struct
import time
import unittest

from tests import test_best1700_bth_qtest as bth_qtest
from tests.test_best1700_peripherals_qtest import QtestMachine


GPIO = 0x6E040000
AON = 0x40080000
RESULT = 0x68001000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcGpioTests(unittest.TestCase):
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

    def test_original_configuration_sequence_is_retained_without_irq(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        self.assertEqual(q.read32(GPIO), 0)
        self.assertEqual(q.read32(GPIO + 0xC), 0)
        # Original cold initialization uses ordinary RMW on unnamed bits 1:2.
        for bit, expected in ((2, 2), (4, 6)):
            q.write32(GPIO + 0xC, (q.read32(GPIO + 0xC) & ~bit) | bit)
            self.assertEqual(q.read32(GPIO + 0xC), expected)
        q.write32(GPIO, 0x23620200)
        q.command("clock_step 100000000")
        self.assertEqual(q.read32(GPIO), 0x23620200)
        self.assertEqual(q.read32(GPIO + 0xC), 6)
        self.assertFalse(q.irq(29))
        self.assertNotIn("BTC GPIO config unsupported",
                         q.stderr_path.read_text(errors="replace"))

    def test_unsupported_fields_offsets_and_accesses_preserve_configuration(self):
        q = self.machine()
        q.write32(GPIO, 0x23620200)
        q.write32(GPIO + 0xC, 6)
        # IRQ-side bit 0 must reject the whole write, including changed bit 2.
        # Likewise reject unknown bits before accepting any supported payload.
        for value in (3, 0xA, 0x80000004):
            q.write32(GPIO + 0xC, value)
            self.assertEqual(q.read32(GPIO + 0xC), 6)
        for offset in (4, 8, 0x10, 0xFFC):
            q.write32(GPIO + offset, 0x12345678)
            q.read32(GPIO + offset)
        for offset in (0, 0xC):
            q.command(f"writeb {GPIO + offset:#x} 0")
            q.command(f"writew {GPIO + offset:#x} 0")
            q.write32(GPIO + offset + 1, 0)
        self.assertEqual(q.read32(GPIO), 0x23620200)
        self.assertEqual(q.read32(GPIO + 0xC), 6)
        log = q.stderr_path.read_text(errors="replace")
        self.assertIn("BTC GPIO config unsupported write offset=0xc value=0x3",
                      log)
        for offset in (4, 8, 0x10, 0xFFC):
            self.assertIn(f"BTC GPIO config unsupported write offset={offset:#x}",
                          log)
            self.assertIn(f"BTC GPIO config unsupported read offset={offset:#x}",
                          log)

    def test_shared_btc_access_and_cpu_versus_machine_reset(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r0, =0x6e040000
    ldr r1, =0x68001000
    ldr r2, [r0]
    str r2, [r1]
    ldr r2, [r0, #0xc]
    str r2, [r1, #4]
    bic r2, r2, #2
    orr r2, r2, #2
    str r2, [r0, #0xc]
    ldr r2, [r0, #0xc]
    bic r2, r2, #4
    orr r2, r2, #4
    str r2, [r0, #0xc]
    ldr r2, =0x23620200
    str r2, [r0]
    movs r2, #1
    str r2, [r1, #8]
1:  wfi
    b 1b
    .ltorg
""", base=0x68004000)
        q = self.machine(tcg=True)
        q.write32(GPIO, 0xA5C35A3C)
        q.write32(GPIO + 0xC, 4)
        q.write(0x68004000, code)
        q.write(0x68002000, struct.pack("<II", 0x68017F00, 0x68004001))
        q.write32(AON + 0x168, 0x68002000)
        q.qmp("cont")
        q.write32(AON + 0xCC, 0x88)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and q.read32(RESULT + 8) != 1:
            time.sleep(0.01)
        q.qmp("stop")
        self.assertEqual(q.read32(RESULT + 8), 1,
                         q.diagnostic("BTC did not finish GPIO config access"))
        self.assertEqual(q.read32(RESULT), 0xA5C35A3C)
        self.assertEqual(q.read32(RESULT + 4), 4)
        self.assertEqual(q.read32(GPIO), 0x23620200)
        self.assertEqual(q.read32(GPIO + 0xC), 6)
        q.write32(AON + 0xC8, 0x80)  # Assert BTC CPU reset only.
        self.assertEqual(q.read32(GPIO), 0x23620200)
        self.assertEqual(q.read32(GPIO + 0xC), 6)
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertEqual(q.read32(GPIO), 0)
        self.assertEqual(q.read32(GPIO + 0xC), 0)


if __name__ == "__main__":
    unittest.main()
