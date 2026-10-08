"""Exercise PSC CPU gating and guest-owned recovery of retained BTC state."""

import json
import os
import struct
import time
import unittest

from tests import test_best1700_btc_sleep_qtest as sleep_qtest


PSC = 0x40085000
CMU = 0x6E030000
MAC = 0x6D040000
SYS = 0x40000000
RESULT = 0x68001000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcPowerTests(unittest.TestCase):
    assemble = sleep_qtest.BtcSleepTests.assemble
    machine = sleep_qtest.BtcSleepTests.machine
    start = sleep_qtest.BtcSleepTests.start

    @staticmethod
    def configure(q, wake=True):
        q.write32(PSC + 0x118, 0x8000 if wake else 0)
        q.write32(PSC + 0x20, 0xCAFE0003)
        q.write32(PSC + 0x30, 0xCAFE0000)
        q.write32(PSC + 0x24, 4)

    def await_word(self, q, address, expected):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if q.read32(address) == expected:
                return
            time.sleep(0.005)
        self.fail(q.diagnostic(f"missing {expected:#x} at {address:#x}"))

    @staticmethod
    def power_gated(q):
        status = json.loads(q.qmp("qom-get", {
            "path": "/machine", "property": "core-status"}))
        return status["btc"] == "power-gated"

    def test_key_and_supported_fields_are_atomic(self):
        q = self.machine()
        self.configure(q)
        for offset, invalid, expected in (
                (0x20, 3, 3), (0x20, 0xCAFE0007, 3),
                (0x24, 0x104, 4), (0x118, 0x8001, 0x8000),
                (0x30, 0xCAFE1000, 0), (0x28, 2, 0), (0x2C, 2, 0)):
            q.write32(PSC + offset, invalid)
            self.assertEqual(q.read32(PSC + offset), expected)
        q.command(f"writew {PSC + 0x20:#x} 0")
        q.command(f"writel {PSC + 0x21:#x} 0xcafe0000")
        self.assertEqual(q.read32(PSC + 0x20), 3)
        self.assertEqual(q.read32(PSC + 0x124), 0)
        q.write32(PSC + 0x124, 0x8000)
        self.assertEqual(q.read32(PSC + 0x124), 0)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset in (0x20, 0x24, 0x118, 0x124):
            self.assertEqual(q.read32(PSC + offset), 0)

    def test_manual_power_sequence_and_start_commands(self):
        q = self.machine()
        # The controller uses a keyed sequence for the individual hardware
        # controls; asserting PSW_EN gates the BTC and clearing it releases it.
        q.write32(PSC + 0x30, 0xCAFE010F)
        self.assertEqual(q.read32(PSC + 0x30), 0x10F)
        self.assertTrue(self.power_gated(q))
        q.write32(PSC + 0x30, 0xCAFE0100)
        self.assertEqual(q.read32(PSC + 0x30), 0x100)
        self.assertFalse(self.power_gated(q))
        q.write32(PSC + 0x28, 1)
        self.assertTrue(self.power_gated(q))
        q.write32(PSC + 0x2C, 1)
        self.assertFalse(self.power_gated(q))
        self.assertNotIn("BTC PSC unsupported",
                          q.stderr_path.read_text(errors="replace"))

    def test_external_wake_respects_disable_and_counts_only_elapsed_lpo(self):
        for elapsed_ns, ticks in ((0, 0), (31_249, 0), (31_250, 1),
                                  (250_000, 8), (999_999, 31)):
            with self.subTest(ns=elapsed_ns):
                q = self.machine()
                q.write32(MAC + 0x18, 2)
                self.start(q, 32000)
                q.step(elapsed_ns)
                q.write32(SYS + 0x140, 0x20)
                self.assertTrue(q.read32(MAC + 0x30) & 0x8000)
                self.assertEqual(q.read32(MAC + 0x1C), 0)
                # A real context restore enables the already pending input.
                q.write32(MAC + 0x30, 0)
                self.assertEqual(q.read32(MAC + 0x38), ticks)
                self.assertEqual(q.read32(MAC + 0x30), 0)
                self.assertEqual(q.read32(MAC + 0x1C), 2)
                self.assertEqual(q.read32(SYS + 0x140) & 0x200020, 0x20)
                q.write32(MAC + 0x20, 2)
                q.step(2_000_000_000)
                self.assertEqual(q.read32(MAC + 0x1C), 0)
                self.assertEqual(q.read32(MAC + 0x38), ticks)

    def test_wake_mask_is_live_and_has_no_ack_side_effect(self):
        q = self.machine()
        self.configure(q, wake=False)
        self.start(q, 32000)
        self.assertTrue(self.power_gated(q))
        q.write32(SYS + 0x140, 0x20)
        self.assertEqual(q.read32(PSC + 0x124), 0)
        self.assertTrue(self.power_gated(q))
        q.write32(PSC + 0x118, 0x8000)
        self.assertEqual(q.read32(PSC + 0x124), 0x8000)
        self.assertFalse(self.power_gated(q))
        q.write32(PSC + 0x118, 0)
        self.assertEqual(q.read32(PSC + 0x124), 0)
        q.write32(PSC + 0x118, 0x8000)
        self.assertEqual(q.read32(SYS + 0x140) & 0x200020, 0x20)
        q.write32(SYS + 0x144, 0x20)
        self.assertEqual(q.read32(PSC + 0x124), 0)
        self.assertEqual(q.read32(SYS + 0x140) & 0x200020, 0x200000)

    def test_pending_wake_releases_gating_and_reset_cancels_off_state(self):
        q = self.machine()
        self.configure(q)
        q.write32(SYS + 0x140, 0x20)
        self.start(q, 32000)
        self.assertFalse(self.power_gated(q))
        q.qmp("system_reset")
        q.qmp("query-status")
        self.configure(q, wake=False)
        self.start(q, 32000)
        self.assertTrue(self.power_gated(q))
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertFalse(self.power_gated(q))
        q.write32(SYS + 0x140, 0x20)
        self.assertEqual(q.read32(PSC + 0x124), 0)
        self.assertEqual(q.read32(MAC + 0x38), 0)

    def test_guest_restarts_at_vector_and_retains_ram_and_ipc(self):
        self.guest_restart(timed=False)

    def test_mttcg_request_before_sleep_still_restarts_through_retained_vector(self):
        self.guest_restart(timed=False, early=True)

    def test_lpo_deadline_restarts_guest_and_allows_context_restore(self):
        self.guest_restart(timed=True)

    def test_dma_completion_irq_survives_power_reset_until_guest_ack(self):
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
    ldr r7, =0x6d010000
    ldr r0, [r4]
    adds r0, #1
    str r0, [r4]
    cmp r0, #2
    beq restored
    movs r1, #1
    str r1, [r7, #0x30]
    ldr r1, =0x68005000
    str r1, [r7, #0x100]
    ldr r1, =0x68006000
    str r1, [r7, #0x104]
    movs r1, #0
    str r1, [r7, #0x108]
    ldr r1, =0x8c000004
    str r1, [r7, #0x10c]
    movw r1, #0x8001
    str r1, [r7, #0x110]
1:  ldr r1, [r7, #0x14]
    cmp r1, #1
    bne 1b
    str r1, [r4, #4]
    movw r1, #64000
    str r1, [r5, #0x34]
    ldr r1, =0x80000007
    str r1, [r5, #0x30]
    ldr r1, [r6, #0x24]
    orr r1, r1, #3
    str r1, [r6, #0x24]
2:  wfi
    b 2b
restored:
    ldr r1, [r7, #0x14]
    str r1, [r4, #8]
    ldr r0, =0xe000e100
    movs r1, #8
    str r1, [r0]
    dsb
    isb
    cpsie i
3:  wfi
    b 3b
    .ltorg
    .org 0x200
.thumb_func
dma_irq:
    ldr r0, =0x6d010000
    ldr r2, =0x68001000
    ldr r1, [r2, #12]
    adds r1, #1
    str r1, [r2, #12]
    mrs r1, ipsr
    str r1, [r2, #16]
    ldr r1, [r0, #4]
    str r1, [r2, #20]
    str r1, [r0, #8]
    ldr r1, [r0, #0x14]
    str r1, [r2, #24]
    movs r1, #0xd5
    str r1, [r2, #28]
    bx lr
    .ltorg
""", base=0x68004000)
        q = self.machine(tcg=True)
        self.configure(q)
        vectors = bytearray(0x100)
        struct.pack_into("<II", vectors, 0, 0x68017F00, 0x68004001)
        struct.pack_into("<I", vectors, (16 + 3) * 4, 0x68004201)
        q.write(0x68002000, vectors)
        q.write(0x68004000, code)
        q.write(0x68005000, b"keep")
        q.write32(0x40080168, 0x68002000)
        q.qmp("cont")
        q.write32(0x400800CC, 0x88)
        deadline = time.monotonic() + 5
        while not self.power_gated(q):
            if time.monotonic() >= deadline:
                self.fail(q.diagnostic("BTC did not gate with DMA pending"))
            time.sleep(0.001)
        self.assertEqual(q.read32(0x6D010014), 1)
        self.assertEqual(q.read32(RESULT + 12), 0)
        # Only the IPC request wakes PSC; no DMA write reasserts IRQ3 after
        # the warm CPU/NVIC reset. The guest alone enables and ACKs IRQ3.
        q.write32(SYS + 0x140, 0x20)
        self.await_word(q, RESULT + 28, 0xD5)
        q.qmp("stop")
        self.assertEqual(struct.unpack("<8I", q.read(RESULT, 32)),
                         (2, 1, 1, 1, 19, 1, 0, 0xD5))
        self.assertEqual(q.read(0x68006000, 4), b"keep")
        self.assertEqual(q.read32(0x6D010000), 0)
        self.assertEqual(q.read32(0x6D010018), 0)
        self.assertFalse(q.irq(3))
        self.assertEqual(q.read32(SYS + 0x140) & 0x200020, 0x20)
        self.assertNotIn("unsupported", q.stderr_path.read_text(errors="replace"))

    def guest_restart(self, *, timed, early=False):
        source = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset
.thumb_func
reset:
    cpsid i
    ldr r4, =0x68001000
    ldr r5, =0x6d040000
    ldr r6, =0x6e030000
    ldr r0, [r4]
    adds r0, #1
    str r0, [r4]
    cmp r0, #2
    beq restored
    ldr r8, =0xabcdef01
    ldr r0, =0x6e060000
    movs r1, #5
    str r1, [r0]
    movw r1, #64000
    str r1, [r5, #0x34]
    ldr r1, =0x80000007
    str r1, [r5, #0x30]
    ldr r1, [r6, #0x24]
    orr r1, r1, #3
    str r1, [r6, #0x24]
1:  wfi
    b 1b
restored:
    str r8, [r4, #4]
    ldr r0, =0x6e060000
    ldr r1, [r0]
    str r1, [r4, #8]
    ldr r0, =0x40000000
    ldr r1, [r0, #0x140]
    str r1, [r4, #12]
    ldr r0, =0x40085000
    ldr r1, [r0, #0x118]
    str r1, [r4, #16]
    movs r1, #0
    str r1, [r5, #0x30]
    ldr r1, [r5, #0x38]
    str r1, [r4, #20]
    movs r1, #0xd5
    str r1, [r4, #24]
2:  wfi
    b 2b
    .ltorg
"""
        if timed:
            source = source.replace("movw r1, #64000", "movw r1, #32")
        code = self.assemble(source, base=0x68004000)
        if early:
            from tests.test_best1700_peripherals_qtest import QtestMachine
            q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                image_payload=b'\xfe\xe7' + bytes(32),
                machine_options=',cp-real-boot=on,bth-real-boot=on,btc-real-boot=on,'
                                'boot-sp=0x203ff000,boot-pc=0x101c0001',
                accel='tcg,thread=multi', extra_args=['-smp', '4', '-S'])
            self.addCleanup(q.close)
        else:
            q = self.machine(tcg=True)
        self.configure(q)
        q.write(0x68002000, struct.pack("<II", 0x68017F00, 0x68004001))
        q.write(0x68004000, code)
        q.write32(0x40080168, 0x68002000)
        if early:
            # Assert before the guest asks PSC to sleep. This is the ordering
            # that previously left the IRQ-masked BTC forever at WFI.
            q.write32(SYS + 0x140, 0x20)
        q.qmp("cont")
        q.write32(0x400800CC, 0x88)
        if not timed and not early:
            deadline = time.monotonic() + 5
            while not self.power_gated(q):
                if time.monotonic() >= deadline:
                    self.fail(q.diagnostic("BTC did not enter PSC power gating"))
                time.sleep(0.001)
            # LPO is two seconds long; the peer request must wake much earlier.
            q.write32(SYS + 0x140, 0x20)
        self.await_word(q, RESULT + 24, 0xD5)
        q.qmp("stop")
        values = struct.unpack("<7I", q.read(RESULT, 28))
        self.assertEqual(values[:3], (2, 0, 5))
        expected_ind = 0 if timed else 0x20
        self.assertEqual(values[3] & 0x200020, expected_ind)
        self.assertEqual(values[4], 0x8000)
        if timed:
            self.assertEqual(values[5], 32)
        else:
            self.assertLess(values[5], 64000)
        self.assertEqual(q.read32(SYS + 0x140) & 0x200020, expected_ind)
        self.assertNotIn("unsupported", q.stderr_path.read_text(errors="replace"))


if __name__ == "__main__":
    unittest.main()
