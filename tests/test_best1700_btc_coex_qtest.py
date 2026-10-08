"""Verify passive BTC coexistence/PCA parameters and reject active requests."""

import os
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


BASEBAND = 0x6D040000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcCoexTests(unittest.TestCase):
    def machine(self):
        q = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=b"\xfe\xe7" + bytes(32),
            machine_options=(",cp-real-boot=on,bth-real-boot=on,btc-real-boot=on"
                             ",boot-sp=0x203ff000,boot-pc=0x101c0001"),
            extra_args=["-smp", "4", "-d", "unimp"])
        self.addCleanup(q.close)
        return q

    def sample(self, q):
        q.write32(BASEBAND + 0x100, 0x80000000)
        return tuple(q.read32(BASEBAND + offset)
                     for offset in (0x100, 0x104, 0x9C4))

    def test_cold_parameters_and_priorities_do_not_start_radio_or_shift_time(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0x80FF)
        q.write32(BASEBAND + 0x40C, 0x10016)
        q.write32(BASEBAND + 0x80C, 0x101FF)
        q.write32(0x68006000, 0x12345678)
        q.write32(BASEBAND + 0xCA4, 0x68006000)
        q.write32(BASEBAND + 0xCAC, 0xAAAAAAAA)
        q.write32(BASEBAND + 0xC04, 0x03000000)
        for offset in (0x540, 0x544, 0x550, 0x564, 0x568):
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        self.assertEqual(self.sample(q), (0, 624, 0))
        # Original PCA cold initializer disables coexistence and sets parameters.
        q.write32(BASEBAND + 0x550, q.read32(BASEBAND + 0x550) & ~4)
        q.write32(BASEBAND + 0x40C, q.read32(BASEBAND + 0x40C) & ~0x600)
        q.write32(BASEBAND + 0x414, 0x700)
        q.write32(BASEBAND + 0x540, 0x10)
        q.write32(BASEBAND + 0x544, 0x00280000)
        # Original post-cold priority RMWs address complete four-bit fields.
        for offset, clear, bits in ((0x564, 0xF00, 0x500),
                                    (0x564, 0xF0000, 0x50000),
                                    (0x568, 0xF0, 0x70),
                                    (0x568, 0xF00, 0x800)):
            q.write32(BASEBAND + offset,
                      (q.read32(BASEBAND + offset) & ~clear) | bits)
        # Merely selecting coexistence or a clock-shift operand is passive.
        q.write32(BASEBAND + 0x550, 4)
        q.write32(BASEBAND + 0x544, 0x002807FF)
        self.assertEqual(self.sample(q), (0, 624, 0))
        q.command("clock_step 100000000")
        self.assertEqual(self.sample(q), (320, 624, 100000))
        for offset, value in ((0x540, 0x10), (0x544, 0x002807FF),
                              (0x550, 4), (0x564, 0x50500), (0x568, 0x870)):
            self.assertEqual(q.read32(BASEBAND + offset), value)
        for offset in (0x1C, 0x410, 0x460, 0x810, 0x860):
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        for irq in (0, 1, 2):
            self.assertFalse(q.irq(irq))
        self.assertEqual(q.read32(0x68006000), 0x12345678)
        self.assertNotIn("baseband unsupported",
                         q.stderr_path.read_text(errors="replace"))

    def test_parameters_do_not_retime_an_existing_target_deadline(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0x80)
        q.write32(BASEBAND + 0xF8, 1)
        q.write32(BASEBAND + 0xFC, 624)  # Due after exactly 312500 ns.
        for offset, value in ((0x540, 0xF0), (0x544, 0x00FF07FF),
                              (0x550, 4), (0x564, 0x000F0F00),
                              (0x568, 0x00000FF0)):
            q.write32(BASEBAND + offset, value)
            self.assertEqual(q.read32(BASEBAND + offset), value)
        q.command("clock_step 312000")
        self.assertEqual(self.sample(q), (0, 0, 312))
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))
        q.command("clock_step 500")
        self.assertEqual(self.sample(q), (1, 624, 312))
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0x80)
        self.assertTrue(q.irq(1))
        self.assertFalse(q.irq(0))
        self.assertFalse(q.irq(2))
        self.assertEqual(q.read32(BASEBAND + 0x410), 0)
        self.assertEqual(q.read32(BASEBAND + 0x810), 0)

    def test_active_requests_and_unknown_fields_reject_atomically_and_reset(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0x80FF)
        cases = (
            (0x540, 0xA0, (0x11, 0x18, 0x12, 0x110, 0x08000010)),
            # Bit 12 starts a real shift; both legal operand and interval change.
            (0x544, 0x002802AA, (0x00301055, 0x00300855, 0x01300055)),
            (0x550, 4, (1, 2, 8, 0x20000000, 0x80000000)),
            (0x564, 0x00050500, (0x000A0A01, 0x000A1A00, 0x001A0A00)),
            (0x568, 0x00000870, (0x00000781, 0x00001780, 0x00010780)),
        )
        before = self.sample(q)
        for offset, original, invalid_values in cases:
            q.write32(BASEBAND + offset, original)
            for invalid in invalid_values:
                q.write32(BASEBAND + offset, invalid)
                self.assertEqual(q.read32(BASEBAND + offset), original)
        self.assertEqual(self.sample(q), before)
        q.command("clock_step 312500")
        self.assertEqual(self.sample(q), (1, 624, 312))
        for offset in (0x1C, 0x410, 0x810):
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        for irq in (0, 1, 2):
            self.assertFalse(q.irq(irq))
        log = q.stderr_path.read_text(errors="replace")
        for offset, _, _ in cases:
            self.assertIn(f"unsupported write offset=0x{offset:x}", log)
        self.assertIn("unsupported write offset=0x544 value=0x301055", log)
        # Configuration survives CPU-only and the documented idle global reset.
        q.write32(0x400800C8, 0x80)
        q.write32(BASEBAND, 0x80000000)
        for offset, original, _ in cases:
            self.assertEqual(q.read32(BASEBAND + offset), original)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset, _, _ in cases:
            self.assertEqual(q.read32(BASEBAND + offset), 0)


if __name__ == "__main__":
    unittest.main()
