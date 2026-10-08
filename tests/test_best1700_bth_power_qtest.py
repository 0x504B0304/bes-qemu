"""Exercise retained BTH power transitions with synthetic Thumb programs.

PSC sequencing delays are functional boundaries, not silicon timing claims.
Guests choose their warm vectors and acknowledge their own pending interrupts.
Every context closes its QEMU before the next subtest creates another one.
"""

from contextlib import contextmanager
import json
import os
import struct
import time
import unittest

from tests.test_best1700_bth_qtest import BthTests
from tests.test_best1700_peripherals_qtest import QtestMachine


PSC = 0x40085000
KEY = 0xCAFE0000
AUTO, STATE, DOWN, UP, MANUAL = (0x34, 0x38, 0x3C, 0x40, 0x44)
WAKE_MASK, WAKE_STATUS = 0x130, 0x13C
AON = 0x40080000
BTH_CMU = 0x50000000
SYS_SET, SYS_CLEAR = 0x40000098, 0x4000009C
TIMER2 = 0x4008B000
RESULT, VECTOR, CODE = 0x23001000, 0x23002000, 0x03004000
WAKE_CORE = 1 << 16


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BthPowerTests(unittest.TestCase):
    assemble = BthTests.assemble

    @contextmanager
    def machine(self, *, tcg=False, multi=False):
        args = ["-smp", "3", "-d", "unimp"]
        if tcg or multi:
            args += ["-S"]
            if not multi:
                args += ["-icount", "shift=3,sleep=off"]
        q = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=b"\xfe\xe7" + bytes(32),
            machine_options=",cp-real-boot=on,bth-real-boot=on,"
                            "boot-sp=0x203ff000,boot-pc=0x101c0001",
            accel="tcg,thread=multi" if multi else "tcg" if tcg else "qtest",
            extra_args=args)
        try:
            q.command("irq_intercept_in /machine/core[2]")
            yield q
        finally:
            q.close()

    @staticmethod
    def core_state(q):
        return json.loads(q.qmp("qom-get", {
            "path": "/machine", "property": "core-status"}))["bth"]

    def await_value(self, q, read, expected, description):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if read() == expected:
                return
            time.sleep(0.005)
        self.fail(q.diagnostic(description))

    def await_word(self, q, offset, expected):
        self.await_value(q, lambda: q.read32(RESULT + offset), expected,
                         f"BTH result+{offset:#x} != {expected:#x}")

    def assert_core(self, q, expected):
        # Reset work belongs to the target vCPU and can briefly be pending.
        self.await_value(q, lambda: self.core_state(q), expected,
                         f"BTH core state did not become {expected}")

    def test_key_width_and_unsupported_fields_do_not_mutate_control(self):
        with self.machine() as q:
            q.write32(PSC + AUTO, KEY | 5)
            q.write32(PSC + MANUAL, KEY | 0x100)
            q.write32(PSC + WAKE_MASK, WAKE_CORE)
            for offset, invalid, expected in (
                    (AUTO, 1, 5), (AUTO, 0xCB000001, 5),
                    (AUTO, KEY | 8, 5), (AUTO, KEY | 0x10, 5),
                    (MANUAL, 0x100, 0x100),
                    (MANUAL, KEY | 0x400, 0x100),
                    (WAKE_MASK, 0x10010000, WAKE_CORE)):
                with self.subTest(offset=offset, invalid=invalid):
                    q.write32(PSC + offset, invalid)
                    self.assertEqual(q.read32(PSC + offset), expected)
            for command in (f"writeb {PSC + AUTO:#x} 0",
                            f"writew {PSC + AUTO:#x} 0",
                            f"writel {PSC + AUTO + 1:#x} {KEY:#x}"):
                q.command(command)
                self.assertEqual(q.read32(PSC + AUTO), 5)
            for value in (1, 0xCB000001, KEY | 2):
                q.write32(PSC + DOWN, value)
                q.step(10)
                self.assertEqual(q.read32(PSC + STATE), 0x100)
                self.assertNotEqual(self.core_state(q), "power-gated")

    def test_status_is_read_only_and_commands_are_not_storage(self):
        with self.machine() as q:
            for offset in (STATE, WAKE_STATUS, 0x138, 0x1B8, 0x1DC):
                expected = 0x100 if offset == STATE else 0
                for value in (0, 0xFFFFFFFF):
                    q.write32(PSC + offset, value)
                    self.assertEqual(q.read32(PSC + offset), expected)
            for offset in (0x12C, 0x1B4, 0x1D8):
                q.write32(PSC + offset, 0xA5A55A5A)
                self.assertEqual(q.read32(PSC + offset), 0xA5A55A5A)
            q.write32(PSC + AUTO, KEY | 1)
            q.write32(PSC + DOWN, KEY | 1)
            self.assertEqual(q.read32(PSC + DOWN), 0)
            self.assertEqual(q.read32(PSC + STATE), 0x300)
            q.step(1)
            self.assert_core(q, "power-gated")
            q.write32(PSC + STATE, 0x100)
            self.assertEqual(q.read32(PSC + STATE), 0x300)
            self.assert_core(q, "power-gated")
            q.write32(PSC + UP, KEY | 1)
            self.assertEqual(q.read32(PSC + UP), 0)
            self.assertEqual(q.read32(PSC + STATE), 0x200)
            q.step(1)
            self.assertEqual(q.read32(PSC + STATE), 0x100)
            # PSC cannot release the independent, still asserted CMU reset.
            self.assert_core(q, "held-reset")

    def test_lpu_completion_requires_an_automatic_cycle_and_is_read_only(self):
        with self.machine() as q:
            lpu = BTH_CMU + 0x8C
            self.assertEqual(q.read32(lpu), 0)
            q.write32(lpu, 0xFFFFFFFF)
            self.assertEqual(q.read32(lpu), 0x27F)
            q.write32(PSC + MANUAL, KEY | 0x88)
            q.write32(PSC + MANUAL, KEY)
            q.write32(PSC + UP, KEY | 1)
            q.step(1)
            self.assertEqual(q.read32(lpu), 0x27F)
            q.write32(PSC + AUTO, KEY | 1)
            q.write32(PSC + DOWN, KEY | 1)
            q.step(1)
            self.assertEqual(q.read32(lpu), 0x27F)
            q.write32(PSC + UP, KEY | 1)
            q.step(1)
            self.assertEqual(q.read32(lpu), 0x2FF)  # oscillator, no PLL lock
            q.write32(lpu, 0)
            self.assertEqual(q.read32(lpu), 0x80)
            q.qmp("system_reset")
            q.qmp("query-status")
            self.assertEqual(q.read32(lpu), 0)

    def test_wake_pulse_during_down_is_latched_and_reset_cancels_up(self):
        with self.machine() as q:
            q.write32(BTH_CMU + 0x168, 1)
            q.write32(BTH_CMU + 0x1B0, 0x200)
            q.write32(PSC + WAKE_MASK, WAKE_CORE)
            q.write32(PSC + AUTO, KEY | 1)
            q.write32(PSC + DOWN, KEY | 1)
            q.write32(SYS_SET, 1 << 16)
            self.assertEqual(q.read32(PSC + WAKE_STATUS), WAKE_CORE)
            q.write32(SYS_CLEAR, 1 << 16)
            self.assertEqual(q.read32(PSC + WAKE_STATUS), 0)
            q.step(1)
            self.assertEqual(q.read32(PSC + STATE), 0x200)
            self.assertEqual(self.core_state(q), "power-gated")
            q.step(1)
            self.assertEqual(q.read32(PSC + STATE), 0x100)
            self.assertEqual(self.core_state(q), "held-reset")
            q.write32(PSC + DOWN, KEY | 1)
            q.step(1)
            q.write32(PSC + UP, KEY | 1)
            q.qmp("system_reset")
            q.qmp("query-status")
            q.step(100)
            self.assertEqual(q.read32(PSC + STATE), 0x100)
            self.assertEqual(q.read32(PSC + AUTO), 0)
            self.assertEqual(q.read32(BTH_CMU + 0x8C), 0)
            self.assertEqual(self.core_state(q), "held-reset")

    def test_manual_controls_require_their_matching_drive_bit(self):
        with self.machine() as q:
            q.write(VECTOR, struct.pack("<II", 0x230BFF00, CODE | 1))
            q.write(CODE + 0x20000000, b"\xfe\xe7")
            q.write32(AON + 0xB8, VECTOR)
            q.write32(AON + 0xCC, 0x300)
            self.assert_core(q, "running")
            q.write32(PSC + MANUAL, KEY | 0xF)
            self.assert_core(q, "running")
            q.write32(PSC + MANUAL, KEY | 0x88)  # PSW_REG plus PSW_DR
            self.assert_core(q, "power-gated")
            q.write32(PSC + MANUAL, KEY | 0x80)  # driven on
            self.assert_core(q, "running")
            q.write32(PSC + MANUAL, KEY | 0x11)  # CLK_STOP_REG plus DR
            self.assert_core(q, "clock-stopped")
            q.write32(PSC + MANUAL, KEY | 1)  # no drive: automatic is on
            self.assert_core(q, "running")
            q.write32(PSC + MANUAL, KEY | 0x44)  # RESET_REG plus DR
            self.assert_core(q, "held-reset")
            q.write32(PSC + MANUAL, KEY | 0x40)
            self.assert_core(q, "running")
            q.write32(PSC + AUTO, KEY | 1)
            q.write32(PSC + MANUAL, KEY | 0xF0)  # all manually kept on
            q.write32(PSC + DOWN, KEY | 1)
            q.step(1)
            self.assertNotEqual(self.core_state(q), "power-gated")
            # Removing the overrides exposes the completed automatic state.
            q.write32(PSC + MANUAL, KEY)
            self.assert_core(q, "power-gated")

    def test_timer2_masked_event_survives_and_wakes_without_ack(self):
        for channel in range(2):
            with self.subTest(channel=channel), self.machine() as q:
                timer, irq, wake = TIMER2 + channel * 0x20, 19 + channel, 1 << (5 + channel)
                q.write32(PSC + AUTO, KEY | 1)
                q.write32(PSC + DOWN, KEY | 1)
                q.step(1)
                self.assert_core(q, "power-gated")
                q.write32(timer + 8, 2)
                q.write32(timer, 15)
                q.write32(timer + 8, 0xA3)
                q.step(2_000_000)
                self.assertEqual(q.read32(timer + 0x10), 1)
                self.assertTrue(q.irq(irq))
                self.assertEqual(q.read32(PSC + WAKE_STATUS), 0)
                self.assert_core(q, "power-gated")
                q.write32(PSC + WAKE_MASK, wake)
                self.assertEqual(q.read32(PSC + WAKE_STATUS), wake)
                q.step(1)
                self.assert_core(q, "held-reset")
                for _ in range(2):
                    self.assertEqual(q.read32(PSC + WAKE_STATUS), wake)
                    q.write32(PSC + WAKE_STATUS, wake)
                    self.assertEqual(q.read32(timer + 0x10), 1)
                    self.assertTrue(q.irq(irq))
                q.write32(timer + 0x0C, 1)
                self.assertEqual(q.read32(PSC + WAKE_STATUS), 0)
                self.assertFalse(q.irq(irq))

    def test_reset_cancels_transitions_and_clears_all_retained_masks(self):
        with self.machine() as q:
            for finish_down in (False, True, False):
                with self.subTest(finish_down=finish_down):
                    q.write32(PSC + AUTO, KEY | 5)
                    for offset in (WAKE_MASK, 0x12C, 0x1B4, 0x1D8):
                        q.write32(PSC + offset, 0x55)
                    q.write32(PSC + DOWN, KEY | 1)
                    if finish_down:
                        q.step(1)
                        self.assert_core(q, "power-gated")
                    q.qmp("system_reset")
                    q.qmp("query-status")
                    q.step(1_000_000)
                    self.assertEqual(q.read32(PSC + STATE), 0x100)
                    self.assert_core(q, "held-reset")
                    for offset in (AUTO, MANUAL, DOWN, UP, WAKE_MASK,
                                   WAKE_STATUS, 0x12C, 0x138, 0x1B4,
                                   0x1B8, 0x1D8, 0x1DC):
                        self.assertEqual(q.read32(PSC + offset), 0)

    def guest(self, *, cmu_mask=True):
        # All addresses below belong to the synthetic fixture or SoC MMIO.
        # The cold path changes its own reset vector before asking PSC to stop.
        source = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    cpsid i
    ldr r4, =0x23001000
    ldr r5, =0x40085000
    ldr r6, =0x50000000
    ldr r0, [r4]
    adds r0, #1
    str r0, [r4]
    ldr r0, [r5, #0x34]
    str r0, [r4, #28]
    ldr r0, =0xcafe0001
    str r0, [r5, #0x34]
    ldr r0, =0x23002000
    ldr r1, =warm_entry
    str r1, [r0, #4]
    movs r0, #1
    str r0, [r6, #0x168]
    movw r0, #CMU_WAKE_MASK
    str r0, [r6, #0x1b0]
    ldr r8, =0xabcdef01
    movs r0, #0xa1
    str r0, [r4, #4]
1:  ldr r0, [r4, #8]
    cmp r0, #0
    beq 1b
    ldr r0, =0xcafe0001
    str r0, [r5, #0x3c]
    dsb
2:  ldr r0, [r4, #24]
    adds r0, #1
    str r0, [r4, #24]
    b 2b
    .ltorg
.org 0x200
.thumb_func
warm_entry:
    cpsid i
    ldr r4, =0x23001000
    ldr r0, [r4, #12]
    adds r0, #1
    str r0, [r4, #12]
    str r8, [r4, #16]
    ldr r0, =0x50000174
    ldr r1, [r0]
    str r1, [r4, #20]
    ldr r0, =0x500001b0
    ldr r1, [r0]
    str r1, [r4, #44]
    ldr r0, =0xe000e108
    movw r1, #0x200
    str r1, [r0]
    dsb
    isb
    cpsie i
3:  wfi
    b 3b
    .ltorg
.org 0x300
.thumb_func
ipc_irq:
    ldr r0, =0x23001000
    ldr r1, [r0, #32]
    adds r1, #1
    str r1, [r0, #32]
    mrs r1, ipsr
    str r1, [r0, #36]
    ldr r1, =0x4000009c
    mov.w r2, #0x10000
    str r2, [r1]
    ldr r1, =0x50000174
    ldr r2, [r1]
    str r2, [r0, #40]
    movs r1, #0xd5
    str r1, [r0, #48]
    bx lr
    .ltorg
"""
        return self.assemble(source.replace("CMU_WAKE_MASK", "512" if cmu_mask else "0"), base=CODE)

    def launch_guest(self, q, code, *, wake=True):
        vectors = bytearray(0x200)
        struct.pack_into("<II", vectors, 0, 0x230BFF00, CODE | 1)
        struct.pack_into("<I", vectors, (16 + 73) * 4, (CODE + 0x300) | 1)
        q.write(RESULT, bytes(64))
        q.write(VECTOR, vectors)
        q.write(CODE + 0x20000000, code)
        q.write32(PSC + AUTO, KEY | 5)
        q.write32(PSC + WAKE_MASK, WAKE_CORE if wake else 0)
        q.write32(AON + 0xB8, VECTOR)
        q.qmp("cont")
        q.write32(AON + 0xCC, 0x300)
        self.await_word(q, 4, 0xA1)
        self.assertEqual(q.read32(RESULT + 28), 5)  # AP write -> BTH read
        self.assertEqual(q.read32(PSC + AUTO), 1)  # BTH write -> AP read
        self.assertEqual(q.read32(VECTOR + 4), (CODE + 0x200) | 1)

    def assert_halted(self, q):
        self.await_value(q, lambda: self.core_state(q), "power-gated",
                         "BTH did not enter power-gated state")
        before = q.read32(RESULT + 24)
        time.sleep(0.025)
        self.assertEqual(q.read32(RESULT + 24), before,
                         "BTH executed instructions while PSC power was off")
        self.assertEqual(q.read32(RESULT + 12), 0)

    def assert_restored(self, q, *, cmu_mask=True):
        self.await_word(q, 48, 0xD5)
        q.qmp("stop")
        self.assertEqual(q.read32(RESULT), 1, "cold entry ran again")
        self.assertEqual(q.read32(RESULT + 12), 1)
        self.assertEqual(q.read32(RESULT + 16), 0, "CPU registers did not reset")
        self.assertEqual(q.read32(RESULT + 20), 0x00010001,
                         "warm reset consumed the pending IPC indication")
        self.assertEqual(q.read32(RESULT + 32), 1)
        self.assertEqual(q.read32(RESULT + 36), 16 + 73)
        self.assertEqual(q.read32(RESULT + 40), 0)
        self.assertEqual(q.read32(RESULT + 44), 0x200 if cmu_mask else 0)
        self.assertEqual(q.read32(PSC + AUTO), 1)
        self.assertEqual(q.read32(PSC + STATE), 0x100)
        self.assertEqual(q.read32(PSC + WAKE_STATUS), 0)
        self.assertFalse(q.irq(73))
        self.assertNotIn("BTH PSC unsupported", q.stderr_path.read_text(errors="replace"))

    def test_guest_warm_vector_retains_pending_ipc_and_survives_two_resets(self):
        code = self.guest()
        with self.machine(tcg=True) as q:
            for cycle in range(3):
                with self.subTest(cycle=cycle):
                    if cycle:
                        q.qmp("system_reset")
                        q.qmp("query-status")
                    self.launch_guest(q, code)
                    q.write32(RESULT + 8, 1)
                    self.assert_halted(q)
                    q.write32(SYS_SET, 1 << 16)
                    self.assert_restored(q)

    def test_masked_ipc_stays_pending_until_psc_mask_is_enabled(self):
        code = self.guest()
        with self.machine(tcg=True) as q:
            self.launch_guest(q, code, wake=False)
            q.write32(RESULT + 8, 1)
            self.assert_halted(q)
            q.write32(SYS_SET, 1 << 16)
            self.assert_halted(q)
            self.assertEqual(q.read32(BTH_CMU + 0x174), 0x00010001)
            self.assertEqual(q.read32(PSC + WAKE_STATUS), 0)
            q.write32(PSC + WAKE_MASK, WAKE_CORE)
            self.assert_restored(q)

    def test_cmu_mask_blocks_core_wake_but_manual_up_retains_pending_irq(self):
        code = self.guest(cmu_mask=False)
        with self.machine(tcg=True) as q:
            self.launch_guest(q, code)
            q.write32(RESULT + 8, 1)
            self.assert_halted(q)
            q.write32(SYS_SET, 1 << 16)
            self.assert_halted(q)
            self.assertEqual(q.read32(PSC + WAKE_STATUS), 0)
            q.write32(PSC + UP, KEY | 1)
            self.assert_restored(q, cmu_mask=False)

    def test_clock_gate_resumes_context_without_fetching_warm_vector(self):
        code = self.guest()
        with self.machine(tcg=True) as q:
            self.launch_guest(q, code)
            q.write32(PSC + MANUAL, KEY | 0x11)
            self.await_value(q, lambda: self.core_state(q), "clock-stopped",
                             "BTH did not stop its clock")
            q.write32(PSC + MANUAL, KEY)
            self.await_value(q, lambda: self.core_state(q), "running",
                             "BTH did not resume its clock")
            self.assertEqual(q.read32(RESULT + 12), 0)
            # The original context must still read this flag and enter PSC;
            # an incorrect reset would already be waiting in warm_entry.
            q.write32(RESULT + 8, 1)
            self.assert_halted(q)
            q.write32(SYS_SET, 1 << 16)
            self.assert_restored(q)

    def test_cmu_reset_owner_prevents_wake_until_explicit_release(self):
        code = self.guest()
        with self.machine(tcg=True) as q:
            self.launch_guest(q, code)
            q.write32(RESULT + 8, 1)
            self.assert_halted(q)
            q.write32(AON + 0xC8, 0x200)
            q.write32(SYS_SET, 1 << 16)
            self.await_value(q, lambda: q.read32(PSC + STATE), 0x100,
                             "PSC did not finish power-up")
            self.await_value(q, lambda: self.core_state(q), "held-reset",
                             "PSC overrode the CMU reset owner")
            self.assertEqual(q.read32(RESULT + 12), 0)
            self.assertEqual(q.read32(BTH_CMU + 0x174), 0x00010001)
            q.write32(AON + 0xCC, 0x200)
            self.assert_restored(q)

    def test_enabling_cmu_mask_releases_an_already_pending_request(self):
        code = self.guest(cmu_mask=False)
        with self.machine(tcg=True) as q:
            self.launch_guest(q, code)
            q.write32(RESULT + 8, 1)
            self.assert_halted(q)
            q.write32(SYS_SET, 1 << 16)
            self.assert_halted(q)
            q.write32(BTH_CMU + 0x1B0, 0x200)
            self.assert_restored(q)

    def test_mttcg_pending_request_before_down_still_uses_guest_warm_vector(self):
        code = self.guest()
        with self.machine(multi=True) as q:
            self.launch_guest(q, code)
            q.write32(SYS_SET, 1 << 16)
            self.assertEqual(q.read32(PSC + WAKE_STATUS), WAKE_CORE)
            q.write32(RESULT + 8, 1)
            self.assert_restored(q)


    def test_repeated_direct_timer_wake_reenters_guest_without_machine_reset(self):
        # Each cycle uses a fresh real timer event with CMU wake masks left
        # zero. The guest takes and clears its own IRQ after a warm CPU reset;
        # neither a machine reset nor a host-side interrupt ACK can hide a
        # stale reset generation, lost retained level, or one-shot wake latch.
        source = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    cpsid i
    ldr r4, =0x23001000
    ldr r5, =0x40085000
    ldr r0, [r4]
    adds r0, #1
    str r0, [r4]
    ldr r0, =0x23002000
    ldr r1, =warm_entry
    str r1, [r0, #4]
    ldr r0, =0xcafe0001
    str r0, [r5, #0x34]
    b cycle_ready
    .ltorg
.org 0x100
.thumb_func
warm_entry:
    cpsid i
    ldr r4, =0x23001000
    ldr r5, =0x40085000
    ldr r0, [r4, #12]
    adds r0, #1
    str r0, [r4, #12]
    str r8, [r4, #16]
    ldr r0, =TIMER_RIS
    ldr r1, [r0]
    str r1, [r4, #20]
    ldr r0, =0x5000008c
    ldr r1, [r0]
    str r1, [r4, #44]
    ldr r0, =0xe000e100
    ldr r1, =TIMER_IRQ_BIT
    str r1, [r0]
    dsb
    isb
    cpsie i
1:  ldr r0, [r4, #32]
    ldr r1, [r4, #12]
    cmp r0, r1
    beq cycle_ready
    wfi
    b 1b
cycle_ready:
    cpsid i
    movs r0, #0
    str r0, [r4, #8]
    ldr r0, [r4, #12]
    adds r0, #0xa1
    str r0, [r4, #4]
    ldr r8, =0xabcdef01
2:  ldr r0, [r4, #8]
    cmp r0, #0
    beq 2b
    ldr r0, =0xcafe0001
    str r0, [r5, #0x3c]
    dsb
3:  ldr r0, [r4, #24]
    adds r0, #1
    str r0, [r4, #24]
    b 3b
    .ltorg
.org 0x200
.thumb_func
timer_irq:
    ldr r0, =0x23001000
    ldr r1, [r0, #32]
    adds r1, #1
    str r1, [r0, #32]
    mrs r1, ipsr
    str r1, [r0, #36]
    ldr r1, =TIMER_CLEAR
    movs r2, #1
    str r2, [r1]
    bx lr
    .ltorg
"""
        for channel in range(2):
            irq = 19 + channel
            timer = TIMER2 + channel * 0x20
            guest = source.replace("TIMER_RIS", hex(timer + 0x10))
            guest = guest.replace("TIMER_CLEAR", hex(timer + 0x0C))
            guest = guest.replace("TIMER_IRQ_BIT", hex(1 << irq))
            code = self.assemble(guest, base=CODE)
            with self.subTest(channel=channel), self.machine(multi=True) as q:
                vectors = bytearray(0x200)
                struct.pack_into("<II", vectors, 0, 0x230BFF00, CODE | 1)
                struct.pack_into("<I", vectors, (16 + irq) * 4,
                                 (CODE + 0x200) | 1)
                q.write(RESULT, bytes(64))
                q.write(VECTOR, vectors)
                q.write(CODE + 0x20000000, code)
                q.write32(PSC + WAKE_MASK, 1 << (5 + channel))
                q.write32(AON + 0xB8, VECTOR)
                q.qmp("cont")
                q.write32(AON + 0xCC, 0x300)
                self.await_word(q, 4, 0xA1)
                for cycle in range(1, 4):
                    with self.subTest(cycle=cycle):
                        q.write32(RESULT + 8, 1)
                        self.assert_core(q, "power-gated")
                        self.assertEqual(q.read32(RESULT + 12), cycle - 1)
                        self.assertEqual(q.read32(BTH_CMU + 0x8C) & 0x180, 0)
                        for bank in range(4):
                            self.assertEqual(q.read32(BTH_CMU + 0x1A8 + bank * 4), 0)
                        # Program the always-on channel only after the CPU is
                        # off; it must continue counting in that domain state.
                        q.write32(timer + 8, 2)
                        q.write32(timer, 15)
                        q.write32(timer + 8, 0xA3)
                        self.await_word(q, 4, 0xA1 + cycle)
                        self.assertEqual(q.read32(RESULT), 1, "cold entry reran")
                        self.assertEqual(q.read32(RESULT + 12), cycle)
                        self.assertEqual(q.read32(RESULT + 16), 0,
                                         "warm cycle did not reset CPU registers")
                        self.assertEqual(q.read32(RESULT + 20), 1,
                                         "warm reset consumed the timer event")
                        self.assertEqual(q.read32(RESULT + 32), cycle)
                        self.assertEqual(q.read32(RESULT + 36), 16 + irq)
                        self.assertEqual(q.read32(RESULT + 44) & 0x180, 0x80)
                        self.assertEqual(q.read32(timer + 0x10), 0)
                        self.assertEqual(q.read32(PSC + STATE), 0x100)
                        self.assertEqual(q.read32(PSC + WAKE_STATUS), 0)
                        self.assertFalse(q.irq(irq))
                q.qmp("stop")
                self.assertNotIn("BTH PSC unsupported",
                                 q.stderr_path.read_text(errors="replace"))


if __name__ == "__main__":
    unittest.main()
