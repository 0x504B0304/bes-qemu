"""Exercise timed BTC LPO measurements and the IRQ 6 read-ack model policy.

These are synthetic clock scenarios, not measured oscillator frequencies.
Every test owns its process and uses QEMU virtual time without real firmware.
"""

import os
import struct
import time
import unittest

from tests import test_best1700_bth_qtest as bth_qtest
from tests.test_best1700_peripherals_qtest import QtestMachine


BTC = 0x6E030000
SLEEP_CFG = BTC + 0x24
WAKEUP_MASK = BTC + 0x3C
CLK_OUT = BTC + 0x44
CALIBRATION = BTC + 0x48
IDLE_REQUEST = BTC + 0x4C
CAL_IRQ = 6
CAL_IRQ_ENABLE = 4
DONE = 1 << 31
AON = 0x40080000
RESET_PULSE = AON + 0xC4
RESET_SET = AON + 0xC8
RESET_CLEAR = AON + 0xCC


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcCalibrationTests(unittest.TestCase):
    assemble = bth_qtest.BthTests.assemble

    def machine(self, *, reference_hz=48_000_000, lpo_hz=32_000, tcg=False):
        args = ["-d", "unimp", "-smp", "4"]
        if tcg:
            args += ["-S", "-icount", "shift=3,sleep=off"]
        machine = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=b"\xfe\xe7" + bytes(32),
            machine_options=(
                ",bth-real-boot=on,btc-real-boot=on"
                ",boot-sp=0x203ff000,boot-pc=0x101c0001"
                f",btc-cal-ref-hz={reference_hz},btc-lpo-hz={lpo_hz}"),
            accel="tcg" if tcg else "qtest", extra_args=args)
        self.addCleanup(machine.close)
        machine.command("irq_intercept_in /machine/btc-core")
        return machine

    def assert_only_calibration_irq(self, machine):
        self.assertTrue(machine.irq(CAL_IRQ))
        self.assertEqual({number for number, level in machine.irq_levels.items()
                          if level}, {CAL_IRQ})

    def test_frequencies_boundaries_and_fractional_rounding(self):
        # Expected deadlines and counters are fixed independent examples.
        # The last case exercises wide arithmetic and the 31-bit count limit.
        cases = (
            (48_000_000, 32_000, 1, 31_250, 1_500),
            (48_000_000, 32_000, 50, 1_562_500, 75_000),
            (48_000_000, 32_000, 256, 8_000_000, 384_000),
            (48_000_000, 32_768, 1, 30_518, 1_464),
            (48_000_000, 32_768, 256, 7_812_500, 375_000),
            (52_000_000, 32_768, 3, 91_553, 4_760),
            (32_000, 32_000, 1, 31_250, 1),
            (1_000_000_000, 1_000_000, 256, 256_000, 256_000),
            (8_388_607, 1, 256, 256_000_000_000, 2_147_483_392),
        )
        for reference, lpo, cycles, duration, count in cases:
            with self.subTest(reference_hz=reference, lpo_hz=lpo, cycles=cycles):
                q = self.machine(reference_hz=reference, lpo_hz=lpo)
                q.write32(CLK_OUT, 0xABCD0000 | (cycles - 1))
                q.write32(SLEEP_CFG, CAL_IRQ_ENABLE)
                q.write32(CALIBRATION, 1)
                self.assertEqual(q.read32(CALIBRATION), 0)
                self.assertFalse(q.irq(CAL_IRQ))
                q.step(duration - 1)
                self.assertEqual(q.read32(CALIBRATION), 0)
                self.assertFalse(q.irq(CAL_IRQ))
                q.step(1)
                self.assert_only_calibration_irq(q)
                self.assertEqual(q.read32(CALIBRATION), DONE | count)
                self.assertFalse(q.irq(CAL_IRQ))
                self.assertEqual(q.read32(CLK_OUT), 0xABCD0000 | (cycles - 1))

    def test_completion_pending_survives_mask_and_late_unmask(self):
        q = self.machine()
        q.write32(CLK_OUT, 49)
        q.write32(CALIBRATION, 1)
        q.step(1_562_500)
        self.assertFalse(q.irq(CAL_IRQ))
        q.write32(SLEEP_CFG, 0x15540 | CAL_IRQ_ENABLE)
        self.assertEqual(q.read32(SLEEP_CFG), 0x15544)
        self.assert_only_calibration_irq(q)
        q.write32(SLEEP_CFG, 0x15540)
        self.assertFalse(q.irq(CAL_IRQ))
        q.write32(SLEEP_CFG, 0x15544)
        self.assert_only_calibration_irq(q)
        self.assertEqual(q.read32(CALIBRATION), DONE | 75_000)
        self.assertFalse(q.irq(CAL_IRQ))
        self.assertEqual(q.read32(CALIBRATION), DONE | 75_000)
        q.write32(SLEEP_CFG, 0x15540)
        q.write32(SLEEP_CFG, 0x15544)
        self.assertFalse(q.irq(CAL_IRQ), "read ACK must not reassert on unmask")

    def test_mask_changes_do_not_cancel_measurement_and_masked_read_acks(self):
        q = self.machine()
        q.write32(CLK_OUT, 49)
        q.write32(SLEEP_CFG, CAL_IRQ_ENABLE)
        q.write32(CALIBRATION, 1)
        q.step(500_000)
        q.write32(SLEEP_CFG, 0)
        self.assertEqual(q.read32(CALIBRATION), 0)
        q.step(1_062_500)
        self.assertFalse(q.irq(CAL_IRQ))
        self.assertEqual(q.read32(CALIBRATION), DONE | 75_000)
        q.write32(SLEEP_CFG, CAL_IRQ_ENABLE)
        self.assertFalse(q.irq(CAL_IRQ), "masked result read acknowledges pending")

    def test_busy_start_rejected_with_captured_count_and_deadline(self):
        q = self.machine()
        q.write32(CLK_OUT, 49)
        q.write32(SLEEP_CFG, CAL_IRQ_ENABLE)
        q.write32(CALIBRATION, 1)
        q.step(500_000)
        q.write32(CLK_OUT, 255)
        q.write32(CALIBRATION, 1)
        q.step(1_062_499)
        self.assertEqual(q.read32(CALIBRATION), 0)
        q.step(1)
        self.assert_only_calibration_irq(q)
        self.assertEqual(q.read32(CALIBRATION), DONE | 75_000)
        self.assertIn("BTC CMU unsupported write offset=0x48 value=0x1",
                      q.stderr_path.read_text(errors="replace"))

        q.write32(CALIBRATION, 1)
        self.assertEqual(q.read32(CALIBRATION), 0)
        q.step(7_999_999)
        self.assertEqual(q.read32(CALIBRATION), 0)
        q.step(1)
        self.assert_only_calibration_irq(q)
        self.assertEqual(q.read32(CALIBRATION), DONE | 384_000)

    def test_new_start_clears_previous_result_and_pending(self):
        q = self.machine()
        q.write32(SLEEP_CFG, CAL_IRQ_ENABLE)
        q.write32(CALIBRATION, 1)
        q.step(31_250)
        self.assert_only_calibration_irq(q)
        # No result read before restarting: accepted start clears pending.
        q.write32(CLK_OUT, 49)
        q.write32(CALIBRATION, 1)
        self.assertFalse(q.irq(CAL_IRQ))
        self.assertEqual(q.read32(CALIBRATION), 0)
        q.step(1_562_499)
        self.assertFalse(q.irq(CAL_IRQ))
        q.step(1)
        self.assert_only_calibration_irq(q)
        self.assertEqual(q.read32(CALIBRATION), DONE | 75_000)

    def test_invalid_commands_preserve_idle_busy_and_completed_state(self):
        q = self.machine()
        q.write32(CLK_OUT, 49)
        q.write32(SLEEP_CFG, CAL_IRQ_ENABLE)
        rejected = (0, 2, 3, DONE, DONE | 1, 0xFFFFFFFF)
        for value in rejected:
            q.write32(CALIBRATION, value)
        q.step(2_000_000)
        self.assertEqual(q.read32(CALIBRATION), 0)
        self.assertFalse(q.irq(CAL_IRQ))

        q.write32(CALIBRATION, 1)
        q.step(500_000)
        for value in rejected:
            q.write32(CALIBRATION, value)
        q.step(1_062_499)
        self.assertEqual(q.read32(CALIBRATION), 0)
        q.step(1)
        self.assert_only_calibration_irq(q)
        for value in rejected:
            q.write32(CALIBRATION, value)
            self.assert_only_calibration_irq(q)
        self.assertEqual(q.read32(CALIBRATION), DONE | 75_000)
        self.assertFalse(q.irq(CAL_IRQ))
        for value in rejected:
            q.write32(CALIBRATION, value)
            self.assertEqual(q.read32(CALIBRATION), DONE | 75_000)
            self.assertFalse(q.irq(CAL_IRQ))

    def test_bad_width_alignment_and_sleep_writes_do_not_ack_or_start(self):
        q = self.machine()
        q.write32(SLEEP_CFG, CAL_IRQ_ENABLE)
        q.command(f"writeb {CALIBRATION:#x} 1")
        q.command(f"writew {CALIBRATION:#x} 1")
        q.command(f"writel {CALIBRATION + 1:#x} 1")
        q.step(31_250)
        self.assertFalse(q.irq(CAL_IRQ))
        self.assertEqual(q.read32(CALIBRATION), 0)
        q.write32(CALIBRATION, 1)
        q.step(31_250)
        self.assert_only_calibration_irq(q)
        for access in (f"readb {CALIBRATION:#x}",
                       f"readw {CALIBRATION:#x}",
                       f"readl {CALIBRATION + 1:#x}",
                       f"writeb {CALIBRATION:#x} 1",
                       f"writew {CALIBRATION:#x} 1",
                       f"writel {CALIBRATION + 1:#x} 1"):
            q.command(access)
            self.assert_only_calibration_irq(q)
        q.write32(SLEEP_CFG, 1)
        self.assertEqual(q.read32(SLEEP_CFG), CAL_IRQ_ENABLE)
        self.assert_only_calibration_irq(q)
        self.assertEqual(q.read32(CALIBRATION), DONE | 1_500)

    def test_machine_reset_cancels_pending_and_inflight_measurements(self):
        q = self.machine()
        for completed in (False, True):
            with self.subTest(completed=completed):
                q.write32(CLK_OUT, 49)
                q.write32(SLEEP_CFG, CAL_IRQ_ENABLE)
                q.write32(WAKEUP_MASK, 0xFFFFFFFF)
                q.write32(CALIBRATION, 1)
                q.step(1_562_500 if completed else 500_000)
                self.assertEqual(q.irq(CAL_IRQ), completed)
                q.qmp("system_reset")
                q.qmp("query-status")
                for register in (CALIBRATION, CLK_OUT, SLEEP_CFG, WAKEUP_MASK):
                    self.assertEqual(q.read32(register), 0)
                self.assertFalse(q.irq(CAL_IRQ))
                q.write32(SLEEP_CFG, CAL_IRQ_ENABLE)
                q.step(2_000_000)
                self.assertEqual(q.read32(CALIBRATION), 0)
                self.assertFalse(q.irq(CAL_IRQ))
                # Reset must clear busy as well as cancel the old timer.
                q.write32(CALIBRATION, 1)
                q.step(31_250)
                self.assert_only_calibration_irq(q)
                self.assertEqual(q.read32(CALIBRATION), DONE | 1_500)

    def test_cpu_reset_preserves_measurement_and_completed_result(self):
        q = self.machine()
        q.write32(CLK_OUT, 49)
        q.write32(SLEEP_CFG, CAL_IRQ_ENABLE)
        q.write32(CALIBRATION, 1)
        q.step(500_000)
        q.write32(RESET_CLEAR, 0x80)
        q.write32(RESET_SET, 0x80)
        q.write32(RESET_PULSE, 0x80)
        self.assertEqual(q.read32(CLK_OUT), 49)
        self.assertEqual(q.read32(SLEEP_CFG), CAL_IRQ_ENABLE)
        q.step(1_062_499)
        self.assertEqual(q.read32(CALIBRATION), 0)
        q.step(1)
        self.assert_only_calibration_irq(q)
        q.write32(RESET_PULSE, 0x80)
        self.assert_only_calibration_irq(q)
        self.assertEqual(q.read32(CALIBRATION), DONE | 75_000)
        q.write32(RESET_PULSE, 0x80)
        self.assertEqual(q.read32(CALIBRATION), DONE | 75_000)
        self.assertFalse(q.irq(CAL_IRQ))

    def test_wakeup_mask_is_full_width_retention_without_sleep_effects(self):
        q = self.machine()
        for value in (0xFFFFFFFF, 0x80000000, 0xAAAAAAAA, 0x55555555, 0):
            q.write32(WAKEUP_MASK, value)
            self.assertEqual(q.read32(WAKEUP_MASK), value)
            self.assertEqual(q.read32(CALIBRATION), 0)
            self.assertEqual(q.read32(SLEEP_CFG), 0)
            self.assertFalse(q.irq(CAL_IRQ))
        q.write32(WAKEUP_MASK, 0x12345678)
        q.write32(RESET_PULSE, 0x80)
        self.assertEqual(q.read32(WAKEUP_MASK), 0x12345678)
        q.command(f"writeb {WAKEUP_MASK:#x} 0")
        q.command(f"writew {WAKEUP_MASK:#x} 0")
        self.assertEqual(q.read32(WAKEUP_MASK), 0x12345678)

    def test_idle_request_is_limited_retention_without_clock_or_irq_effects(self):
        q = self.machine()
        self.assertEqual(q.read32(IDLE_REQUEST), 0)
        for value in (0, 2, 3, DONE, 0xFFFFFFFF):
            q.write32(IDLE_REQUEST, value)
            self.assertEqual(q.read32(IDLE_REQUEST), 0)
        q.write32(BTC, 0x8008)
        q.write32(SLEEP_CFG, CAL_IRQ_ENABLE)
        q.write32(WAKEUP_MASK, 0x55555555)
        q.write32(IDLE_REQUEST, 1)
        q.write32(IDLE_REQUEST, 1)
        self.assertEqual(q.read32(IDLE_REQUEST), 1)
        self.assertEqual(q.read32(BTC), 0x8008)
        self.assertEqual(q.read32(WAKEUP_MASK), 0x55555555)
        self.assertEqual(q.read32(SLEEP_CFG), CAL_IRQ_ENABLE)
        self.assertEqual(q.read32(CALIBRATION), 0)
        self.assertFalse(q.irq(CAL_IRQ))
        q.write32(CALIBRATION, 1)
        q.step(31_250)
        self.assert_only_calibration_irq(q)
        for value in (0, 2, 3, DONE, 0xFFFFFFFF):
            q.write32(IDLE_REQUEST, value)
            self.assertEqual(q.read32(IDLE_REQUEST), 1)
            self.assert_only_calibration_irq(q)
        q.write32(IDLE_REQUEST, 1)
        self.assert_only_calibration_irq(q)
        self.assertEqual(q.read32(CALIBRATION), DONE | 1_500)
        q.write32(RESET_PULSE, 0x80)
        self.assertEqual(q.read32(IDLE_REQUEST), 1)
        q.command(f"writeb {IDLE_REQUEST:#x} 0")
        q.command(f"writew {IDLE_REQUEST:#x} 0")
        self.assertEqual(q.read32(IDLE_REQUEST), 1)
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertEqual(q.read32(IDLE_REQUEST), 0)

    def test_btc_wfi_wakes_on_irq6_and_guest_result_read_acknowledges(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x68001000
    ldr r5, =0x6e030000
    movs r0, #49
    str r0, [r5, #0x44]
    movs r0, #4
    str r0, [r5, #0x24]
    ldr r1, =0xe000e100
    movs r0, #64
    str r0, [r1]
    movs r0, #1
    str r0, [r5, #0x48]
    ldr r0, =0x8008
    str r0, [r5, #4]
    movs r0, #1
    str r0, [r5, #0x4c]
    str r0, [r4]
    wfi
    ldr r0, =0x8008
    str r0, [r5]
    movs r0, #2
    str r0, [r4]
1:  wfi
    b 1b
    .ltorg
    .org 0x100
.thumb_func
calibration_irq:
    ldr r0, =0x6e030048
    ldr r1, [r0]
    ldr r0, =0x68001004
    str r1, [r0]
    bic r1, r1, #0x80000000
    ldr r2, =48000000
    movs r3, #50
    mul r2, r2, r3
    udiv r2, r2, r1
    str r2, [r0, #4]
    bx lr
    .ltorg
""", base=0x68004000)
        q = self.machine(tcg=True)
        q.write(0x68004000, code)
        vectors = [0] * 23
        vectors[0] = 0x68017F00
        vectors[1] = 0x68004001
        vectors[16 + CAL_IRQ] = 0x68004101
        q.write(0x68002000, struct.pack("<23I", *vectors))
        q.write32(AON + 0x168, 0x68002000)
        q.write32(BTC, 0x8008)
        q.qmp("cont")
        q.write32(RESET_CLEAR, 0x88)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if q.read32(0x68001000) == 2:
                break
            time.sleep(0.01)
        else:
            self.fail(q.diagnostic("BTC did not wake from WFI on calibration IRQ"))
        self.assertEqual(q.read32(0x68001004), DONE | 75_000)
        self.assertEqual(q.read32(0x68001008), 32_000)
        self.assertEqual(q.read32(CALIBRATION), DONE | 75_000)
        self.assertEqual(q.read32(BTC), 0x8008)
        self.assertEqual(q.read32(IDLE_REQUEST), 1)
        self.assertFalse(q.irq(CAL_IRQ))


if __name__ == "__main__":
    unittest.main()
