"""Exercise BTC timed sleep, retained CPU state and both wake IRQ stages.

Synthetic programs use guest MMIO and real virtual-time deadlines. The
312500 ns correction latency is explicit model policy, not measured silicon.
No test injects firmware responses, radio packets or interrupt completions.
"""

import os
import struct
import subprocess
import time
import unittest

from tests import test_best1700_bth_qtest as bth_qtest
from tests.test_best1700_peripherals_qtest import QtestMachine


MAC = 0x6D040000
CMU = 0x6E030000
SYS = 0x40000000
IRQ_ENABLE = MAC + 0x18
IRQ_STATUS = MAC + 0x1C
IRQ_ACK = MAC + 0x20
SLEEP_CONTROL = MAC + 0x30
SLEEP_COUNT = MAC + 0x34
SLEEP_ELAPSED = MAC + 0x38
FINE_CORRECTION = MAC + 0x40
SLOT_CORRECTION = MAC + 0x44
ISO_CONTROL = MAC + 0x9C0
ISO_CORRECTION = MAC + 0x9C8
ISO_CORRECTION_HUS = MAC + 0x9CC
EXT_DISABLED = 1 << 31
SLEEPING = 1 << 15
SLEEP_START = 4
CORRECT = 8
DELTA = 1 << 31
WAKE_IRQ = 2
CORRECTION_IRQ = 1
TARGET3_IRQ = 0x80
CORRECTION_NS = 312_500


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcSleepTests(unittest.TestCase):
    assemble = bth_qtest.BthTests.assemble

    def machine(self, *, lpo_hz=32_000, tcg=False, correction_ns=CORRECTION_NS):
        args = ["-smp", "4", "-d", "unimp"]
        if tcg:
            args += ["-S", "-icount", "shift=3,sleep=off"]
        q = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=b"\xfe\xe7" + bytes(32),
            machine_options=(
                ",cp-real-boot=on,bth-real-boot=on,btc-real-boot=on"
                ",boot-sp=0x203ff000,boot-pc=0x101c0001"
                f",btc-lpo-hz={lpo_hz},btc-correction-ns={correction_ns}"),
            accel="tcg" if tcg else "qtest", extra_args=args)
        self.addCleanup(q.close)
        q.command("irq_intercept_in /machine/btc-core")
        return q

    @staticmethod
    def sample(q):
        q.write32(MAC + 0x100, 1 << 31)
        return (q.read32(MAC + 0x100) * 625 +
                624 - q.read32(MAC + 0x104), q.read32(MAC + 0x9C4))

    def start(self, q, count):
        q.write32(SLEEP_COUNT, count)
        q.write32(SLEEP_CONTROL, EXT_DISABLED | 3 | SLEEP_START)
        self.assertEqual(q.read32(SLEEP_CONTROL) & (SLEEPING | SLEEP_START),
                         SLEEPING | SLEEP_START)
        self.assertTrue(q.read32(CMU + 0x24) & (1 << 31))
        # The original guest carries the read-only sync bit through this RMW.
        q.write32(CMU + 0x24, q.read32(CMU + 0x24) | 3)
        self.assertEqual(q.read32(CMU + 0x24) & 3, 2)

    @staticmethod
    def correct(q, *, slots, fine, iso_us, iso_hus=0):
        q.write32(SLOT_CORRECTION, DELTA | slots)
        q.write32(FINE_CORRECTION, fine)
        q.write32(ISO_CONTROL, 1)
        q.write32(ISO_CORRECTION, iso_us)
        q.write32(ISO_CORRECTION_HUS, iso_hus)
        q.write32(SLEEP_CONTROL, q.read32(SLEEP_CONTROL) | CORRECT)

    def assert_pending(self, q, mask):
        self.assertEqual(q.read32(IRQ_STATUS), mask)
        self.assertEqual(q.irq(1), bool(mask))

    def test_correction_latency_rejects_invalid_startup_and_runtime_changes(self):
        for latency in (0, 1_000_000_001):
            result = subprocess.run([os.environ['MIBAND_BEST1700_QEMU'],
                '-M', f'miband-best1700,btc-correction-ns={latency}',
                '-display', 'none', '-monitor', 'none'], capture_output=True,
                text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('btc-correction-ns must be between', result.stderr)
        q = self.machine(correction_ns=2_000_000)
        self.assertEqual(q.qmp('qom-get', dict(path='/machine', property='btc-correction-ns')), 2_000_000)
        with self.assertRaisesRegex(RuntimeError, 'startup-only'):
            q.qmp('qom-set', dict(path='/machine', property='btc-correction-ns', value=1))
        self.assertEqual(q.qmp('qom-get', dict(path='/machine', property='btc-correction-ns')), 2_000_000)

    def test_configured_latency_preserves_masked_completion_and_final_ack(self):
        latency = 2_000_000
        q = self.machine(correction_ns=latency)
        self.start(q, 32)
        q.step(1_000_000)
        q.write32(IRQ_ACK, WAKE_IRQ)
        self.correct(q, slots=3, fine=499, iso_us=1000)
        # Reproduce a guest delayed 600us before its stale IRQ clear.
        q.step(600_000)
        q.write32(IRQ_ACK, CORRECTION_IRQ)
        q.step(latency - 600_000 - 1)
        self.assert_pending(q, 0)
        q.step(1)
        # Completion latches even when masked; it is not an enable-triggered ACK.
        self.assert_pending(q, 0)
        self.assertEqual(self.sample(q), (6000, 3000))
        q.write32(IRQ_ENABLE, CORRECTION_IRQ)
        self.assert_pending(q, CORRECTION_IRQ)
        q.write32(IRQ_ACK, CORRECTION_IRQ)
        q.write32(IRQ_ENABLE, 0)
        q.write32(IRQ_ENABLE, CORRECTION_IRQ)
        q.write32(MAC + 0xd00, 1)
        q.step(latency)
        self.assert_pending(q, 0)
        # Full machine reset retains startup latency but cancels old events.
        q.qmp('system_reset')
        self.start(q, 32)
        q.step(1_000_000)
        q.write32(IRQ_ACK, WAKE_IRQ)
        self.correct(q, slots=0, fine=624, iso_us=0)
        q.step(latency - 1)
        q.write32(IRQ_ENABLE, CORRECTION_IRQ)
        self.assert_pending(q, 0)
        q.step(1)
        self.assert_pending(q, CORRECTION_IRQ)

    def test_programming_operands_does_not_start_and_fields_are_atomic(self):
        q = self.machine()
        q.write32(IRQ_ENABLE, 3)
        q.write32(CMU + 0x24, 0x12C44)
        for count in (1, 0xF9CB, 0xFFFFFFFF):
            q.write32(SLEEP_COUNT, count)
            self.assertEqual(q.read32(SLEEP_COUNT), count)
        q.write32(SLEEP_CONTROL, EXT_DISABLED | 3)
        q.step(2_000_000)
        self.assertEqual(q.read32(SLEEP_ELAPSED), 0)
        self.assertEqual(q.read32(SLEEP_CONTROL), EXT_DISABLED | 3)
        self.assertEqual(q.read32(CMU + 0x24), 0x12C44)
        self.assert_pending(q, 0)
        # A CMU start without the MAC handshake must not change configuration.
        q.write32(CMU + 0x24, 0x12C47)
        self.assertEqual(q.read32(CMU + 0x24), 0x12C44)
        q.write32(FINE_CORRECTION, 499)
        q.write32(SLOT_CORRECTION, DELTA | 3)
        q.write32(ISO_CONTROL, 1)
        q.write32(ISO_CORRECTION_HUS, 1)
        for register, value, expected in (
                (SLEEP_CONTROL, EXT_DISABLED | 3 | 0x100, EXT_DISABLED | 3),
                (FINE_CORRECTION, 1024, 499),
                (SLOT_CORRECTION, DELTA | 0x10000003, DELTA | 3),
                (ISO_CONTROL, 0x80000001, 1),
                (ISO_CORRECTION_HUS, 3, 1)):
            q.write32(register, value)
            self.assertEqual(q.read32(register), expected)
        q.write32(SLEEP_ELAPSED, 0xFFFFFFFF)
        self.assertEqual(q.read32(SLEEP_ELAPSED), 0)
        for command in (f"writeb {SLEEP_COUNT:#x} 1",
                        f"writew {SLEEP_COUNT:#x} 1",
                        f"writel {SLEEP_COUNT + 1:#x} 1"):
            q.command(command)
            self.assertEqual(q.read32(SLEEP_COUNT), 0xFFFFFFFF)
        q.write32(SLEEP_CONTROL, EXT_DISABLED | 3 | CORRECT)
        q.step(CORRECTION_NS)
        self.assert_pending(q, 0)

    def test_lpo_deadlines_use_count_and_frequency_and_latch_elapsed(self):
        # Independent known deadlines include non-integral nanoseconds and
        # the full-width count; no test copies the model's conversion formula.
        cases = ((32_000, 1, 31_250), (32_000, 32, 1_000_000),
                 (32_000, 0xF9CB, 1_998_343_750),
                 (32_768, 1, 30_518), (32_768, 256, 7_812_500),
                 (32_000, 0xFFFFFFFF, 134_217_727_968_750))
        for frequency, count, deadline in cases:
            with self.subTest(frequency=frequency, count=count):
                q = self.machine(lpo_hz=frequency)
                q.write32(IRQ_ENABLE, WAKE_IRQ)
                self.start(q, count)
                q.step(deadline - 1)
                self.assert_pending(q, 0)
                self.assertTrue(q.read32(SLEEP_CONTROL) & SLEEPING)
                q.step(1)
                self.assert_pending(q, WAKE_IRQ)
                self.assertEqual(q.read32(SLEEP_ELAPSED), count)
                self.assertEqual(q.read32(SLEEP_CONTROL) &
                                 (SLEEPING | SLEEP_START), 0)
                q.write32(SLEEP_ELAPSED, 0)
                q.step(1_000_000)
                self.assertEqual(q.read32(SLEEP_ELAPSED), count)
                self.assert_pending(q, WAKE_IRQ)

    def test_mac_restore_preserves_only_unacknowledged_sleep_events(self):
        for phase in ("sleep", "wake", "correcting", "corrected"):
            with self.subTest(phase=phase):
                q = self.machine()
                q.write32(IRQ_ENABLE, WAKE_IRQ | CORRECTION_IRQ)
                self.start(q, 32)
                if phase != "sleep":
                    q.step(1_000_000)
                if phase in ("correcting", "corrected"):
                    q.write32(IRQ_ACK, WAKE_IRQ)
                    self.correct(q, slots=3, fine=499, iso_us=1000)
                if phase == "corrected":
                    q.step(CORRECTION_NS)
                expected = {"sleep": 0, "wake": WAKE_IRQ,
                            "correcting": 0, "corrected": CORRECTION_IRQ}[phase]
                elapsed = q.read32(SLEEP_ELAPSED)
                control = q.read32(SLEEP_CONTROL)
                q.write32(SYS + 0x140, 0x20)
                q.write32(IRQ_ENABLE, 0)
                q.write32(MAC + 0xD00, 1)
                self.assert_pending(q, 0)
                q.write32(IRQ_ENABLE, WAKE_IRQ | CORRECTION_IRQ)
                self.assert_pending(q, expected)
                self.assertEqual(q.read32(SLEEP_ELAPSED), elapsed)
                self.assertEqual(q.read32(SLEEP_CONTROL), control)
                self.assertEqual(q.read32(SYS + 0x140) & 0x20, 0x20)
                if phase in ("sleep", "correcting"):
                    q.step(1_000_000 if phase == "sleep" else CORRECTION_NS)
                    expected = WAKE_IRQ if phase == "sleep" else CORRECTION_IRQ
                    self.assert_pending(q, expected)
                # ACK is final. Repeating context restore cannot rearm an IRQ,
                # even while the sequencer still needs guest clock correction.
                q.write32(IRQ_ACK, expected)
                q.write32(MAC + 0xD00, 1)
                q.step(2_000_000)
                self.assert_pending(q, 0)

    def test_both_events_survive_masks_and_original_ack_order(self):
        q = self.machine()
        q.step(125_000)
        before = self.sample(q)
        self.assertEqual(before, (250, 125))
        self.assertEqual(q.read32(MAC + 0xe78), 125)
        self.start(q, 32)
        q.step(1_000_000)
        self.assert_pending(q, 0)
        self.assertEqual(self.sample(q), before)
        self.assertEqual(q.read32(MAC + 0xe78), 125)
        q.write32(IRQ_ENABLE, WAKE_IRQ)
        self.assert_pending(q, WAKE_IRQ)
        q.write32(IRQ_ENABLE, 0)
        self.assert_pending(q, 0)
        q.write32(IRQ_ENABLE, WAKE_IRQ)
        self.assert_pending(q, WAKE_IRQ)
        q.write32(IRQ_ACK, WAKE_IRQ)
        self.correct(q, slots=3, fine=499, iso_us=1000)
        # Original IRQ handler requests correction, then ACKs and enables 1.
        q.write32(IRQ_ACK, CORRECTION_IRQ)
        q.write32(IRQ_ENABLE, CORRECTION_IRQ)
        q.step(CORRECTION_NS - 1)
        self.assert_pending(q, 0)
        q.write32(IRQ_ENABLE, 0)
        q.step(1)
        self.assert_pending(q, 0)
        # The high-frequency clock runs after LPO wake: account for the
        # correction latency once, as well as the guest's sleep delta.
        self.assertEqual(self.sample(q), (2875, 1437))
        self.assertEqual(q.read32(MAC + 0xe78), 1437)
        q.write32(IRQ_ENABLE, CORRECTION_IRQ)
        self.assert_pending(q, CORRECTION_IRQ)
        q.write32(IRQ_ACK, CORRECTION_IRQ)
        q.write32(IRQ_ENABLE, 0)
        q.write32(IRQ_ENABLE, 3)
        q.step(CORRECTION_NS)
        self.assert_pending(q, 0)
        self.assertEqual(q.read32(SLEEP_CONTROL) & (SLEEP_START | CORRECT), 0)

    def test_iso_half_microsecond_is_a_fraction_not_a_high_word(self):
        q = self.machine()
        self.start(q, 1)
        q.step(31_250)
        q.write32(IRQ_ACK, WAKE_IRQ)
        # Deliberately distinct native/ISO operands prove that the ISO
        # correction consumes its own guest values, including the HUS bit.
        self.correct(q, slots=0, fine=562, iso_us=17, iso_hus=1)
        q.step(CORRECTION_NS)
        self.assertEqual(self.sample(q), (687, 330))
        q.step(500)
        self.assertEqual(self.sample(q), (688, 330))
        q.step(500)
        self.assertEqual(self.sample(q), (689, 331))

    def test_correction_preserves_running_subtick_and_target_phase(self):
        q = self.machine()
        q.write32(IRQ_ENABLE, TARGET3_IRQ)
        # Target native half-microsecond 1000, a tick boundary.
        q.write32(MAC + 0xF8, 1)
        q.write32(MAC + 0xFC, 249)
        self.start(q, 1)
        q.step(31_250)
        q.step(250)
        self.correct(q, slots=0, fine=624, iso_us=0)
        q.step(CORRECTION_NS)
        self.assertEqual(self.sample(q), (625, 312))
        q.step(249)
        self.assertEqual(self.sample(q), (625, 312))
        q.step(1)
        self.assertEqual(self.sample(q), (626, 313))
        q.step(186_999)
        self.assertFalse(q.irq(1))
        q.step(1)
        self.assert_pending(q, TARGET3_IRQ)

    def test_busy_or_invalid_requests_cannot_replace_inflight_operations(self):
        q = self.machine()
        q.write32(IRQ_ENABLE, 3)
        self.start(q, 32)
        q.step(250_000)
        control = q.read32(SLEEP_CONTROL)
        q.write32(SLEEP_COUNT, 320)
        q.write32(SLEEP_CONTROL, control | SLEEP_START)
        q.write32(SLEEP_CONTROL, control | CORRECT)
        q.write32(SLEEP_CONTROL, control | 0x100)
        self.assertEqual(q.read32(SLEEP_COUNT), 32)
        self.assertEqual(q.read32(SLEEP_CONTROL), control)
        q.step(749_999)
        self.assert_pending(q, 0)
        q.step(1)
        self.assert_pending(q, WAKE_IRQ)
        self.assertEqual(q.read32(SLEEP_ELAPSED), 32)
        q.write32(IRQ_ACK, WAKE_IRQ)
        # Invalid fine value cannot destroy the previously programmed value.
        q.write32(FINE_CORRECTION, 499)
        q.write32(FINE_CORRECTION, 625)
        self.assertEqual(q.read32(FINE_CORRECTION), 499)
        self.correct(q, slots=3, fine=499, iso_us=1000)
        q.step(100_000)
        q.write32(SLEEP_CONTROL, EXT_DISABLED | 3 | CORRECT)
        q.write32(SLEEP_CONTROL, EXT_DISABLED | 3 | SLEEP_START)
        q.write32(SLOT_CORRECTION, DELTA | 100)
        q.write32(FINE_CORRECTION, 0)
        q.write32(ISO_CORRECTION, 0xFFFFFFFF)
        q.write32(ISO_CORRECTION_HUS, 1)
        q.step(CORRECTION_NS - 100_001)
        self.assert_pending(q, 0)
        q.step(1)
        self.assert_pending(q, CORRECTION_IRQ)
        self.assertEqual(self.sample(q), (2625, 1312))

    def test_target3_stays_frozen_until_correction_then_uses_native_phase(self):
        q = self.machine()
        q.write32(MAC + 0xF8, 25)
        q.write32(MAC + 0xFC, 249)  # 8000 us from the reset native epoch.
        q.write32(IRQ_ENABLE, WAKE_IRQ | CORRECTION_IRQ | TARGET3_IRQ)
        q.step(1_000_000)
        self.start(q, 128)  # Four ms of LPO time.
        q.step(4_000_000)
        self.assert_pending(q, WAKE_IRQ)
        self.assertEqual(self.sample(q), (2000, 1000))
        q.write32(IRQ_ACK, WAKE_IRQ)
        self.correct(q, slots=12, fine=124, iso_us=4000)
        q.step(CORRECTION_NS)
        self.assertEqual(self.sample(q), (10625, 5312))
        self.assert_pending(q, CORRECTION_IRQ)
        q.write32(IRQ_ACK, CORRECTION_IRQ)
        q.step(2_687_499)
        self.assert_pending(q, 0)
        q.step(1)
        self.assert_pending(q, TARGET3_IRQ)
        q.write32(IRQ_ACK, TARGET3_IRQ)
        q.step(8_000_000)
        self.assert_pending(q, 0)

    def test_reset_cancels_sleep_wake_and_correction_without_stale_events(self):
        for reset_kind in ("machine", "mac"):
            for state in ("sleep", "wake", "correction"):
                with self.subTest(reset=reset_kind, state=state):
                    q = self.machine()
                    q.write32(IRQ_ENABLE, 3)
                    self.start(q, 32)
                    q.step(250_000 if state == "sleep" else 1_000_000)
                    if state == "correction":
                        q.write32(IRQ_ACK, WAKE_IRQ)
                        self.correct(q, slots=3, fine=499, iso_us=1000)
                        q.step(100_000)
                    if reset_kind == "machine":
                        q.qmp("system_reset")
                        q.qmp("query-status")
                    else:
                        q.write32(MAC, 1 << 31)
                    self.assertEqual(q.read32(SLEEP_CONTROL) &
                                     (SLEEPING | SLEEP_START | CORRECT), 0)
                    self.assertEqual(q.read32(CMU + 0x24) & (1 << 31), 0)
                    q.write32(IRQ_ENABLE, 3)
                    q.step(2_000_000)
                    self.assert_pending(q, 0)
                    self.start(q, 1)
                    q.step(31_250)
                    self.assert_pending(q, WAKE_IRQ)
                    self.assertEqual(q.read32(SLEEP_ELAPSED), 1)

    def test_sleep_and_correction_do_not_acknowledge_pending_ipc(self):
        q = self.machine()
        q.write32(SYS + 0x140, 1 << 13)  # CP channel 1 DONE enable.
        self.start(q, 32)
        q.write32(SYS + 0x140, 0x20)  # CP -> BTC channel 1 indication.
        self.assertEqual(q.read32(CMU + 0xA0) & 0x800000, 0x800000)
        self.assertEqual(q.read32(SYS + 0x140) & 0x200000, 0)
        q.step(1_000_000)
        q.write32(IRQ_ACK, WAKE_IRQ)
        self.correct(q, slots=3, fine=499, iso_us=1000)
        q.step(CORRECTION_NS)
        self.assertEqual(q.read32(CMU + 0xA0) & 0x800000, 0x800000)
        self.assertEqual(q.read32(SYS + 0x140) & 0x200000, 0)
        q.write32(SYS + 0x144, 0x20)  # Only a real guest ACK creates DONE.
        self.assertEqual(q.read32(CMU + 0xA0) & 0x800000, 0)
        self.assertEqual(q.read32(SYS + 0x140) & 0x200000, 0x200000)

    def test_synthetic_btc_wfi_preserves_context_and_handles_both_irq1_events(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    cpsid i
    ldr r4, =0x68001000
    ldr r5, =0x6d040000
    ldr r6, =0x6e030000
    ldr r8, =0x3579bdf1
    ldr r0, =0xe000e100
    movs r1, #2
    str r1, [r0]
    str r1, [r5, #0x18]
    movs r1, #32
    str r1, [r5, #0x34]
    ldr r1, =0x80000007
    str r1, [r5, #0x30]
1:  ldr r1, [r6, #0x24]
    cmp r1, #0
    bge 1b
    orr r1, r1, #3
    str r1, [r6, #0x24]
    movs r1, #1
    str r1, [r4]
    str r1, [r6, #0x4c]
    cpsie i
2:  ldr r1, [r4, #4]
    cmp r1, #2
    beq 3f
    wfi
    b 2b
3:  str r8, [r4, #24]
    movs r1, #0xd5
    str r1, [r4]
4:  wfi
    b 4b
    .ltorg
    .org 0x200
.thumb_func
baseband_irq:
    ldr r0, =0x6d040000
    ldr r2, =0x68001000
    ldr r1, [r0, #0x1c]
    mrs r3, ipsr
    str r3, [r2, #12]
    cmp r1, #2
    bne correction_complete
    str r1, [r2, #16]
    str r1, [r0, #0x20]
    ldr r1, [r0, #0x38]
    str r1, [r2, #8]
    ldr r1, =0x80000003
    str r1, [r0, #0x44]
    movw r1, #499
    str r1, [r0, #0x40]
    movs r1, #1
    str r1, [r0, #0x9c0]
    movw r1, #1000
    str r1, [r0, #0x9c8]
    movs r1, #0
    str r1, [r0, #0x9cc]
    ldr r1, [r0, #0x30]
    orr r1, r1, #8
    str r1, [r0, #0x30]
    movs r1, #1
    str r1, [r0, #0x20]
    str r1, [r0, #0x18]
    b count_irq
correction_complete:
    str r1, [r2, #20]
    str r1, [r0, #0x20]
count_irq:
    ldr r1, [r2, #4]
    adds r1, r1, #1
    str r1, [r2, #4]
    bx lr
    .ltorg
""", base=0x68004000)
        q = self.machine(tcg=True)
        vectors = bytearray(0x100)
        struct.pack_into("<II", vectors, 0, 0x68017F00, 0x68004001)
        struct.pack_into("<I", vectors, (16 + 1) * 4, 0x68004201)
        q.write(0x68002000, vectors)
        q.write(0x68004000, code)
        q.write32(0x40080168, 0x68002000)
        q.write32(CMU, 0x8008)
        q.qmp("cont")
        q.write32(0x400800CC, 0x88)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if q.read32(0x68001000) == 0xD5:
                break
            time.sleep(0.01)
        else:
            self.fail(q.diagnostic("BTC did not complete both sleep wake IRQs"))
        q.qmp("stop")
        self.assertEqual(struct.unpack("<7I", q.read(0x68001000, 28)),
                         (0xD5, 2, 32, 17, WAKE_IRQ, CORRECTION_IRQ, 0x3579BDF1))
        self.assert_pending(q, 0)
        self.assertNotIn("Lockup", q.stderr_path.read_text(errors="replace"))


if __name__ == "__main__":
    unittest.main()
