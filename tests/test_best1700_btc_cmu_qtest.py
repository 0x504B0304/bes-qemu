"""Exercise BTC clock/reset state and AON-controlled controller execution.

Tests use their own synthetic images and processes. Unknown BTC registers
remain diagnostic; no controller response or firmware-ready state is supplied.
"""

import os
import struct
import time
import unittest

from tests import test_best1700_bth_qtest as bth_qtest
from tests.test_best1700_peripherals_qtest import QtestMachine


BTC = 0x6E030000
AON = 0x40080000
RESET_PULSE = AON + 0xC4
RESET_SET = AON + 0xC8
RESET_CLEAR = AON + 0xCC
VTOR = AON + 0x168
RESET_MASK = 0x88
RESULT = 0x68001000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcCmuTests(unittest.TestCase):
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

    def test_clock_commands_preserve_other_gates_and_reset_commands_are_active_low(self):
        machine = self.machine()
        machine.write32(BTC, 0x8008)
        machine.write32(BTC, 0x200)
        self.assertEqual(machine.read32(BTC), 0x8208)
        self.assertEqual(machine.read32(BTC + 4), 0x8208)
        machine.write32(BTC + 4, 0x8008)
        self.assertEqual(machine.read32(BTC), 0x200)
        self.assertEqual(machine.read32(BTC + 4), 0x200)
        machine.write32(BTC, 0)
        machine.write32(BTC + 4, 0)
        self.assertEqual(machine.read32(BTC), 0x200)

        machine.write32(BTC + 0x14, 0x208)
        machine.write32(BTC + 0x14, 0x8000)
        self.assertEqual(machine.read32(BTC + 0x10), 0x8208)
        self.assertEqual(machine.read32(BTC + 0x14), 0x8208)
        machine.write32(BTC + 0x10, 8)
        self.assertEqual(machine.read32(BTC + 0x14), 0x8200)
        machine.write32(BTC + 0x10, 0)
        machine.write32(BTC + 0x14, 0)
        self.assertEqual(machine.read32(BTC + 0x10), 0x8200)
        self.assertEqual(machine.read32(BTC), 0x200)

    def test_module_init_rmw_and_configuration_survive_cpu_reset(self):
        machine = self.machine()
        machine.write32(BTC + 8, 0x40)
        machine.write32(BTC + 8, machine.read32(BTC + 8) | 0x200)
        machine.write32(BTC + 0x2C, 0x18)
        machine.write32(BTC + 0x2C, machine.read32(BTC + 0x2C) | 0x1C0)
        self.assertEqual(machine.read32(BTC + 8), 0x240)
        self.assertEqual(machine.read32(BTC + 0x2C), 0x1D8)
        machine.write32(BTC + 0x38, 0x100)
        machine.write32(BTC + 0x38, machine.read32(BTC + 0x38) | 0x80)
        machine.write32(BTC + 0x38, machine.read32(BTC + 0x38) & ~0x100)
        self.assertEqual(machine.read32(BTC + 0x38), 0x80)
        for offset in (0x0C, 0x18, 0x44):
            machine.write32(BTC + offset, 0x12345678)
            self.assertEqual(machine.read32(BTC + offset), 0x12345678)
        machine.write32(BTC + 0xA8, 0xFFFFFFFD)
        self.assertEqual(machine.read32(BTC + 0xA8), 5)
        machine.write32(RESET_CLEAR, 0x80)
        machine.write32(RESET_SET, 0x80)
        self.assertEqual(machine.read32(BTC + 8), 0x240)
        self.assertEqual(machine.read32(BTC + 0x2C), 0x1D8)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        for offset in (0, 4, 8, 0x0C, 0x10, 0x14, 0x18, 0x24, 0x2C, 0x38, 0x44, 0xA8):
            self.assertEqual(machine.read32(BTC + offset), 0)

    def test_sleep_configuration_preserves_rmw_without_executing_commands(self):
        machine = self.machine()
        sleep_config = BTC + 0x24
        self.assertEqual(machine.read32(sleep_config), 0)
        machine.write32(sleep_config, machine.read32(sleep_config) | 0x40)
        machine.write32(sleep_config, machine.read32(sleep_config) | 0x10000)
        for wait_cycles in (1, 255, 0, 85):
            value = (machine.read32(sleep_config) & ~0xFF00) | (wait_cycles << 8)
            machine.write32(sleep_config, value)
            self.assertEqual(machine.read32(sleep_config), 0x10040 | (wait_cycles << 8))

        # All documented configuration fields are retained; START remains a
        # command and SYNC is a read-only handshake bit.
        config = 0x15540 | 0x70000 | 0x03000000
        machine.write32(sleep_config, config)
        self.assertEqual(machine.read32(sleep_config), config & 0x6B07FFFE)
        machine.write32(sleep_config, config | 0x80000)
        self.assertEqual(machine.read32(sleep_config), config & 0x6B07FFFE)
        machine.write32(sleep_config, config | 1)
        self.assertEqual(machine.read32(sleep_config), config & 0x6B07FFFE)
        machine.write32(sleep_config, config | 0x800000000)
        self.assertEqual(machine.read32(sleep_config), config & 0x6B07FFFE)

        machine.write32(sleep_config, 0x15540)
        machine.command(f"writeb {sleep_config:#x} 0")
        machine.command(f"writew {sleep_config:#x} 0")
        self.assertEqual(machine.read32(sleep_config), 0x15540)
        machine.write32(RESET_CLEAR, 0x80)
        machine.write32(RESET_SET, 0x80)
        self.assertEqual(machine.read32(sleep_config), 0x15540)
        self.assertIn("BTC CMU unsupported write offset=0x24",
                      machine.stderr_path.read_text(errors="replace"))
        machine.qmp("system_reset")
        machine.qmp("query-status")
        self.assertEqual(machine.read32(sleep_config), 0)

    def test_pvt_counter_uses_window_and_reports_done(self):
        machine = self.machine()
        pvt_cfg = BTC + 0xCC
        pvt_count = BTC + 0xD0
        controls = 0xF | (100 << 4)

        machine.write32(pvt_cfg, controls)
        self.assertEqual(machine.read32(pvt_cfg), controls)
        self.assertEqual(machine.read32(pvt_count), 0)
        machine.step(99_000)
        self.assertEqual(machine.read32(pvt_count), 0)
        machine.step(1_000)
        expected = (48_000_000 * 100 // 1_000_000) | (1 << 18)
        self.assertEqual(machine.read32(pvt_count), expected)

        machine.write32(pvt_cfg, 0)
        self.assertEqual(machine.read32(pvt_count), 0)
        machine.write32(pvt_cfg, 0x8000 | controls)
        self.assertEqual(machine.read32(pvt_cfg), 0)
        self.assertIn("BTC CMU unsupported write offset=0xcc",
                      machine.stderr_path.read_text(errors="replace"))

    def test_btc_reset_pulse_restarts_only_supported_reset_lines(self):
        machine = self.machine()
        machine.write32(BTC + 0x08, 0x240)
        machine.write32(BTC + 0x1C, 0x88)
        machine.write32(BTC + 0x1C, 0x08)
        self.assertEqual(machine.read32(BTC + 0x08), 0x240)
        machine.write32(BTC + 0x1C, 0x01)
        self.assertEqual(machine.read32(BTC + 0x08), 0x240)
        self.assertIn("BTC CMU unsupported write offset=0x1c",
                      machine.stderr_path.read_text(errors="replace"))

    def test_extended_cmu_registers_follow_sdk_set_clear_semantics(self):
        q = self.machine()
        q.write32(BTC + 0x20, 0xA5A5A5A5)
        q.write32(BTC + 0x28, 0x68401234)
        q.write32(BTC + 0x30, 0xFFFFFFFF)
        self.assertEqual(q.read32(BTC + 0x20), 0xA5A5A5A5)
        self.assertEqual(q.read32(BTC + 0x28), 0x68401234)
        self.assertEqual(q.read32(BTC + 0x30), 0x7FFFF)

        q.write32(BTC + 0x40, 1)
        self.assertEqual(q.read32(BTC + 0x40), 2)
        q.write32(BTC + 0x40, 0)
        self.assertEqual(q.read32(BTC + 0x40), 0)

        q.write32(BTC + 0x58, 0x11)
        q.write32(BTC + 0x5C, 0x22)
        q.write32(BTC + 0x60, 0x05)
        q.write32(BTC + 0x60, 0x20)
        q.write32(BTC + 0x64, 0x01)
        self.assertEqual(q.read32(BTC + 0x58), 0x11)
        self.assertEqual(q.read32(BTC + 0x5C), 0x22)
        self.assertEqual(q.read32(BTC + 0x60), 0x24)
        q.write32(BTC + 0x64, 0x24)
        self.assertEqual(q.read32(BTC + 0x60), 0)

        q.write32(BTC + 0x70, 0x31)
        q.write32(BTC + 0x74, 0x32)
        q.write32(BTC + 0x78, 0x40)
        q.write32(BTC + 0x7C, 0x10)
        q.write32(BTC + 0x80, 0x80)
        q.write32(BTC + 0x84, 0x20)
        q.write32(BTC + 0x88, 0x43)
        self.assertEqual(q.read32(BTC + 0x70), 0x31)
        self.assertEqual(q.read32(BTC + 0x74), 0x32)
        self.assertEqual(q.read32(BTC + 0x78), 0x40)
        self.assertEqual(q.read32(BTC + 0x80), 0x80)
        self.assertEqual(q.read32(BTC + 0x88), 0x43)

        q.write32(BTC + 0xC0, 0x301)
        self.assertEqual(q.read32(BTC + 0xC0), 0x301)
        q.write32(BTC + 0xD4, 0x5)
        q.write32(BTC + 0xD4, 0x8)
        q.write32(BTC + 0xD8, 0x1)
        q.write32(BTC + 0xDC, 0x12)
        q.write32(BTC + 0xE0, 0x9)
        q.write32(BTC + 0xE4, 0x1)
        self.assertEqual(q.read32(BTC + 0xDC), 0x12)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset in (0x20, 0x28, 0x30, 0x40, 0x58, 0x60, 0x70,
                       0x78, 0x80, 0x88, 0xC0, 0xD4, 0xDC, 0xE0):
            self.assertEqual(q.read32(BTC + offset), 0)

    def test_unsupported_accesses_do_not_become_configuration_or_ready(self):
        machine = self.machine()
        machine.write32(BTC + 8, 0x200)
        machine.command(f"writeb {BTC + 8:#x} 0xff")
        machine.command(f"writew {BTC + 8:#x} 0xffff")
        machine.command(f"writel {BTC + 9:#x} 0xffffffff")
        self.assertEqual(machine.read32(BTC + 8), 0x200)
        machine.write32(BTC + 0x2C, 0x1C0)
        machine.write32(BTC + 0x2C, 7)
        self.assertEqual(machine.read32(BTC + 0x2C), 0x1C0)
        machine.write32(BTC + 0x3FC, 0xFFFFFFFF)
        self.assertEqual(machine.read32(BTC + 0x3FC), 0)
        log = machine.stderr_path.read_text(errors="replace")
        self.assertIn("BTC CMU unsupported write offset=0x3fc", log)
        self.assertIn("BTC CMU unsupported read offset=0x3fc", log)
        self.assertIn("BTC CMU unsupported write offset=0x2c", log)

    def test_original_save_replay_preserves_configuration_and_live_ipc(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        configured = {0x24: 0x15540, 0x34: 0x2405A, 0x38: 0x80,
                      0x3C: 0x80000040, 0x40: 0, 0x44: 49, 0x58: 0,
                      0x8C: 0x8021, 0xA8: 5}
        for offset, value in configured.items():
            q.write32(BTC + offset, value)

        # The save list includes live IPC masks/status, not CMU storage.
        # Assert BTH lane 0 and lane 2 indications before saving either word.
        q.write32(BTC + 0xA0, 4)
        q.write32(BTC + 0xAC, 0x10)
        q.write32(0x500000A0, 4)
        q.write32(0x50000078, 4)
        self.assertTrue(q.irq(14))
        self.assertTrue(q.irq(23))
        offsets = (0x24, 0x34, 0x38, 0x3C, 0x40, 0x44,
                   0x58, 0x8C, 0xA0, 0xA8, 0xAC)
        saved = {offset: q.read32(BTC + offset) for offset in offsets}
        self.assertEqual(saved[0xA0], 0x444)
        self.assertEqual(saved[0xAC], 0x1110)
        for offset in configured:
            q.write32(BTC + offset, 0)
        for offset in offsets:
            q.write32(BTC + offset, saved[offset])
            self.assertEqual(q.read32(BTC + offset), saved[offset])
        self.assertTrue(q.irq(14), "CMU replay must not ACK lane 0")
        self.assertTrue(q.irq(23), "CMU replay must not ACK lane 2")
        q.write32(0x500000A4, 4)
        q.write32(0x5000007C, 4)
        self.assertFalse(q.irq(14))
        self.assertFalse(q.irq(23))
        for offset in (0x34, 0x40, 0x58, 0x8C):
            for operation in ("read", "write"):
                self.assertNotIn(
                    f"BTC CMU unsupported {operation} offset={offset:#x}",
                    q.stderr_path.read_text(errors="replace"))

    def test_saved_controls_reject_active_bits_atomically_without_changing_lpo(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BTC + 0x34, 0x2405A)
        q.write32(BTC + 0x8C, 0x8021)
        q.write32(BTC, 0x8008)
        q.write32(BTC + 0x14, 0x208)
        q.write32(BTC + 0x44, 49)
        q.write32(BTC + 0x24, 4)
        q.write32(BTC + 0x48, 1)
        q.step(500_000)
        for active_bit in (8, 9, 10, 11, 12, 13, 14, 15, 16, 17):
            value = (1 << active_bit) | 0xC0A5
            q.write32(BTC + 0x34, value)
            self.assertEqual(q.read32(BTC + 0x34), value & 0x3FFFF)
        for value in (0x10000, 0x80000000, 0xFFFFFFFF):
            q.write32(BTC + 0x8C, value)
            self.assertEqual(q.read32(BTC + 0x8C), 0x8021)
        for value in (1, 2, 3, 0x80000000, 0xFFFFFFFF):
            q.write32(BTC + 0x40, 0)
            q.write32(BTC + 0x40, value)
            self.assertEqual(q.read32(BTC + 0x40), 2 if value == 1 else 0)
        for value in (1, 2, 3, 0x80000000, 0xFFFFFFFF):
            q.write32(BTC + 0x58, value)
            self.assertEqual(q.read32(BTC + 0x58), value)
        self.assertEqual(q.read32(BTC), 0x8008)
        self.assertEqual(q.read32(BTC + 0x14), 0x208)
        q.step(1_062_499)
        self.assertFalse(q.irq(6))
        self.assertEqual(q.read32(BTC + 0x48), 0)
        q.step(1)
        self.assertTrue(q.irq(6), "rejected writes must preserve the deadline")
        for offset, value in ((0x34, 0x3C0FF), (0x8C, 0xFFFF),
                              (0x40, 0), (0x58, 0)):
            q.write32(BTC + offset, value)
            self.assertEqual(q.read32(BTC + offset), value)
            self.assertTrue(q.irq(6), "save controls must not ACK calibration")
        self.assertEqual(q.read32(BTC + 0x48), 0x800124F8)
        self.assertFalse(q.irq(6))
        q.step(2_000_000)
        for irq in (0, 1, 2, 3, 6, 29):
            self.assertFalse(q.irq(irq))
        log = q.stderr_path.read_text(errors="replace")
        for offset in (0x40, 0x8C):
            self.assertIn(f"BTC CMU unsupported write offset={offset:#x}", log)

    def test_saved_controls_keep_access_width_and_neighbor_boundaries(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BTC + 0x34, 0x2405A)
        q.write32(BTC + 0x8C, 0x8021)
        q.write32(BTC + 0x24, 4)
        q.write32(BTC + 0x48, 1)
        q.step(31_250)
        for offset, expected in ((0x34, 0x2405A), (0x40, 0),
                                 (0x58, 0), (0x8C, 0x8021)):
            address = BTC + offset
            for access in (f"writeb {address:#x} 0",
                           f"writew {address:#x} 0",
                           f"writel {address + 1:#x} 0",
                           f"readb {address:#x}",
                           f"readw {address:#x}",
                           f"readl {address + 1:#x}"):
                q.command(access)
                self.assertEqual(q.read32(address), expected)
                self.assertTrue(q.irq(6))
        expected = {0x30: 0x7FFFF, 0x5C: 0xFFFFFFFF,
                    0x60: 0xFFFFFFFF, 0x64: 0,
                    0x88: 0xFFFFFFFF, 0x90: 0}
        for offset, value in expected.items():
            q.write32(BTC + offset, 0xFFFFFFFF)
            self.assertEqual(q.read32(BTC + offset), value)
            self.assertTrue(q.irq(6))
        self.assertEqual(q.read32(BTC + 0x34), 0x2405A)
        self.assertEqual(q.read32(BTC + 0x8C), 0x8021)
        self.assertEqual(q.read32(BTC + 0x48), 0x800005DC)
        log = q.stderr_path.read_text(errors="replace")
        self.assertIn(
            "BTC CMU unsupported write offset=0x90", log)

    def test_saved_configuration_survives_cpu_reset_and_clears_on_machine_reset(self):
        q = self.machine()
        q.write32(BTC + 0x34, 0x3C0FF)
        q.write32(BTC + 0x8C, 0xFFFF)
        for offset in (0x40, 0x58):
            q.write32(BTC + offset, 0)
        q.write32(0x68006000, 0x12345678)
        for mask in (0x80, 8):
            q.write32(RESET_CLEAR, mask)
            q.write32(RESET_SET, mask)
            q.write32(RESET_PULSE, mask)
            self.assertEqual(q.read32(BTC + 0x34), 0x3C0FF)
            self.assertEqual(q.read32(BTC + 0x8C), 0xFFFF)
            self.assertEqual(q.read32(0x68006000), 0x12345678)
            for offset in (0x40, 0x58):
                self.assertEqual(q.read32(BTC + offset), 0)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset in (0x34, 0x40, 0x58, 0x8C):
            self.assertEqual(q.read32(BTC + offset), 0)
            q.write32(BTC + offset, 0)

    def test_aon_reset_latches_and_vector_byte_lanes_are_independent(self):
        machine = self.machine()
        self.assertEqual(machine.read32(RESET_SET) & RESET_MASK, 0)
        others = machine.read32(RESET_CLEAR) & ~RESET_MASK
        machine.command(f"writeb {RESET_CLEAR:#x} 0x80")
        self.assertEqual(machine.read32(RESET_SET) & RESET_MASK, 0x80)
        machine.command(f"writeb {RESET_CLEAR:#x} 8")
        self.assertEqual(machine.read32(RESET_CLEAR) & RESET_MASK, RESET_MASK)
        machine.write32(RESET_PULSE, RESET_MASK)
        self.assertEqual(machine.read32(RESET_CLEAR) & RESET_MASK, RESET_MASK)
        self.assertEqual(machine.read32(RESET_PULSE) & RESET_MASK, 0)
        machine.write32(RESET_SET, 8)
        self.assertEqual(machine.read32(RESET_CLEAR) & RESET_MASK, 0x80)
        self.assertEqual(machine.read32(RESET_CLEAR) & ~RESET_MASK, others)
        # The endpoint owns aligned vector bits only; low reserved bits may
        # have independent fallback storage in the surrounding AON model.
        machine.write32(VTOR, 0x6800CAFF)
        self.assertEqual(machine.read32(VTOR) & 0xFFFFFF80, 0x6800CA80)
        machine.command(f"writeb {VTOR + 1:#x} 0x23")
        self.assertEqual(machine.read32(VTOR) & 0xFFFFFF80, 0x68002380)

    def test_btc_executes_only_after_both_resets_release_and_pulses_reload_vector(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x68001000
    ldr r0, [r4]
    adds r0, #1
    str r0, [r4]
    ldr r5, =0x6e030000
    ldr r0, [r5, #8]
    str r0, [r4, #4]
    ldr r0, [r5, #0x2c]
    str r0, [r4, #8]
    ldr r0, [r5, #0x24]
    str r0, [r4, #12]
1:  wfi
    b 1b
    .ltorg
    .balign 64
.thumb_func
reset_b:
    ldr r4, =0x68001000
    ldr r0, [r4]
    adds r0, #10
    str r0, [r4]
2:  wfi
    b 2b
    .ltorg
""", base=0x68004000)
        machine = self.machine(tcg=True)
        machine.write(0x68004000, code)
        machine.write(0x68002000, struct.pack("<II", 0x68017F00, 0x68004001))
        machine.write(0x68002100, struct.pack("<II", 0x68017F00, 0x68004041))
        machine.write32(VTOR, 0x68002000)
        machine.write32(BTC + 8, 0x200)
        machine.write32(BTC + 0x2C, 0x1C0)
        machine.write32(BTC + 0x24, 0x15540)

        def await_count(expected):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if machine.read32(RESULT) == expected:
                    return
                time.sleep(0.01)
            self.fail(machine.diagnostic(f"missing BTC reset count {expected}"))

        machine.qmp("cont")
        machine.write32(RESET_CLEAR, 0x80)
        time.sleep(0.05)
        self.assertEqual(machine.read32(RESULT), 0)
        machine.write32(RESET_CLEAR, 8)
        await_count(1)
        self.assertEqual(struct.unpack("<III", machine.read(RESULT + 4, 12)),
                         (0x200, 0x1C0, 0x15540))
        machine.write32(RESET_PULSE, 0x80)
        await_count(2)
        machine.write32(RESET_SET, 8)
        machine.write32(VTOR, 0x68002100)
        machine.write32(RESET_CLEAR, 0x80)
        time.sleep(0.05)
        self.assertEqual(machine.read32(RESULT), 2)
        machine.write32(RESET_CLEAR, 8)
        await_count(12)
        machine.write32(RESET_PULSE, 8)
        await_count(22)
        machine.qmp("stop")
        self.assertEqual(machine.read32(RESET_CLEAR) & RESET_MASK, RESET_MASK)
        self.assertNotIn("Lockup", machine.stderr_path.read_text(errors="replace"))

    def test_controller_crash_dump_pc_is_a_live_cpu_diagnostic(self):
        machine = self.machine()
        machine.write(0x68000000, struct.pack("<II", 0x203ff000, 0x68000101))
        machine.write32(VTOR, 0x68000000)
        machine.write32(RESET_CLEAR, 0x88)
        self.assertEqual(machine.read32(BTC + 0xbc), 0x68000100)
        machine.write32(BTC + 0xbc, 0x12345678)
        self.assertNotEqual(machine.read32(BTC + 0xbc), 0x12345678)


if __name__ == "__main__":
    unittest.main()
