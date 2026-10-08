"""Digital PSRAM controller contracts, independent of external firmware."""
import os
from pathlib import Path
import struct
import tempfile
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


BASE = 0x40320000
PHY = BASE + 0x8000
STATUS = BASE + 0x404
ISPI = 0x4000a000
AUTO_LP = 0x1800


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700PsramQtest(unittest.TestCase):
    def setUp(self):
        self.q = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"])
        self.addCleanup(self.q.close)

    def lp_state(self):
        return (self.q.read32(PHY + 0x44) >> 25) & 15

    def test_phy_power_handshake_requires_controls_and_virtual_time(self):
        q = self.q
        self.assertEqual(self.lp_state(), 0)
        q.write32(PHY + 0x44, 0xffffffff)
        self.assertEqual(self.lp_state(), 0, "status must be read-only")
        q.write32(PHY + 0x40, 0x800)
        q.step(10000)
        self.assertEqual(self.lp_state(), 0)
        q.write32(PHY + 0x40, AUTO_LP | 0x40)
        self.assertEqual(self.lp_state(), 0)
        q.step(999)
        self.assertEqual(self.lp_state(), 0)
        q.step(1)
        self.assertEqual(self.lp_state(), 3)
        q.write32(PHY + 0x40, AUTO_LP | 0xa540)
        self.assertEqual(self.lp_state(), 3, "sample selection must retain power state")
        q.write32(PHY + 0x40, 0)
        self.assertEqual(self.lp_state(), 0)
        q.write32(PHY + 0x40, AUTO_LP)
        q.step(500)
        q.write32(PHY + 0x40, 0)
        q.step(10000)
        self.assertEqual(self.lp_state(), 0, "disabled power request must cancel completion")

    def test_manager_fifo_and_command_busy_lifetime(self):
        q = self.q
        self.assertEqual((q.read32(STATUS) >> 7) & 15, 8)
        for index in range(8):
            q.write32(BASE + 0x14, index + 0x1234)
            self.assertEqual((q.read32(STATUS) >> 7) & 15, 7 - index)
        self.assertTrue(q.read32(STATUS) & 0x40)
        q.write32(BASE + 0x14, 0xdeadbeef)  # Full FIFO rejects a ninth word.
        q.write32(BASE + 0x0c, 0x2008)
        q.write32(BASE + 0x08, 0)
        q.write32(BASE + 0x04, 0x05)
        self.assertTrue(q.read32(STATUS) & 1)
        q.write32(BASE + 0x04, 0x06)  # Busy controller retains its first command.
        q.step(999)
        self.assertTrue(q.read32(STATUS) & 1)
        q.step(1)
        self.assertEqual(q.read32(STATUS) & 0x41, 0)
        self.assertEqual((q.read32(STATUS) >> 7) & 15, 8)
        self.assertTrue(q.read32(STATUS) & 2, "rejected read must not populate RX")
        q.write32(BASE + 0x14, 0xbeef)
        q.write32(BASE + 0x1c, 2)
        self.assertEqual((q.read32(STATUS) >> 7) & 15, 7)
        q.write32(BASE + 0x1c, 1)
        self.assertEqual((q.read32(STATUS) >> 7) & 15, 8)
        self.assertEqual(q.read32(BASE + 0x1c), 0)

    def test_receive_clear_does_not_delay_pending_transmit(self):
        q = self.q
        q.write32(BASE + 0x0c, 0x2001)
        q.write32(BASE + 0x14, 0x1234)
        q.write32(BASE + 0x04, 5)
        q.step(999)
        self.assertTrue(q.read32(STATUS) & 1)
        q.write32(BASE + 0x1c, 2)
        q.step(1)
        self.assertFalse(q.read32(STATUS) & 1)

    def test_dll_lock_tracks_manual_clock_and_reset_controls(self):
        q = self.q
        q.write32(PHY + 0x54, 0x7f)
        self.assertEqual(q.read32(PHY + 0x54), 0)
        q.write32(PHY + 0x50, (1 << 28) | (1 << 29))
        q.write32(PHY + 0x40, AUTO_LP)
        q.step(10000)
        self.assertEqual(q.read32(PHY + 0x54), 0)
        q.write32(PHY + 0x58, 1 << 16)  # Clock only: reset is still asserted.
        q.step(10000)
        self.assertEqual(q.read32(PHY + 0x54), 0)
        q.write32(PHY + 0x58, 0x90000)
        self.assertEqual(q.read32(PHY + 0x54), 0)
        q.step(1000)
        self.assertEqual(q.read32(PHY + 0x54), 0x50)
        q.write32(PHY + 0x58, 1 << 19)  # Clock stops and lock is lost.
        self.assertEqual(q.read32(PHY + 0x54), 0)
        q.write32(PHY + 0x50, 0)  # Return to automatic clock/reset control.
        q.step(1000)
        self.assertEqual(q.read32(PHY + 0x54), 0x50)
        q.write32(PHY + 0x40, 0)
        self.assertEqual(q.read32(PHY + 0x54), 0)

    def test_dll_tap_parameter_exposes_saturation(self):
        for taps, expected in ((0, 0xc0), (63, 0x17f)):
            q = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"], extra_args=[
                "-global", f"best1700-psram.dll-delay-taps={taps}"])
            try:
                q.write32(PHY + 0x40, AUTO_LP)
                q.step(1000)
                q.step(1000)
                self.assertEqual(q.read32(PHY + 0x54), expected)
            finally:
                q.close()

    def test_ispi_endpoint_shares_dll_state_and_keeps_chip_selects_separate(self):
        q = self.q

        def read(reg, chip_select=5):
            q.write32(ISPI + 4, chip_select << 7)
            q.write32(ISPI + 8, (1 << 28) | (reg << 18))
            return q.read32(ISPI + 8) >> 2

        def write(reg, value, chip_select=5):
            q.write32(ISPI + 4, chip_select << 7)
            q.write32(ISPI + 8, (reg << 18) | (value << 2))

        self.assertEqual(read(0x51), 0)
        write(0x51, 0xffff)
        self.assertEqual(read(0x51), 0, "DLL result must be read-only over ISPI")
        q.write32(PHY + 0x40, AUTO_LP)
        q.step(1000)
        q.step(1000)
        self.assertEqual(read(0x51), q.read32(PHY + 0x54))
        self.assertEqual(read(0x51), 0x50)
        write(0x52, (3 << 11) | 0x123)
        self.assertEqual(read(0x52), (3 << 11) | 0x123)
        self.assertEqual((q.read32(PHY + 0x58) >> 17) & 3, 3)
        self.assertEqual(read(0x51), 0, "range change must restart DLL acquisition")
        q.step(1000)
        self.assertEqual(read(0x51), 0x50)
        write(0x50, 0x2345)
        self.assertEqual(q.read32(PHY + 0x58) & 0xffff, 0x2345)
        q.write32(PHY + 0x58, 0x60000 | 0x5678)
        self.assertEqual(read(0x50), 0x5678)
        write(0x52, 0xbeef, chip_select=4)
        self.assertEqual(read(0x52, chip_select=4), 0xbeef)
        self.assertEqual(read(0x52), (3 << 11) | 0x123)
        q.qmp("system_reset")
        q.qmp("query-status")
        q.step(10000)
        self.assertEqual(read(0x51), 0)
        self.assertEqual(read(0x50), 0)
        self.assertEqual(read(0x52), 0)
        q.write32(PHY + 0x40, AUTO_LP)
        q.step(1000)
        q.step(1000)
        self.assertEqual(read(0x51), 0x50, "reset must retain endpoint wiring")

    def test_absent_package_read_is_open_bus_with_fifo_consumption(self):
        q = self.q
        q.write32(BASE + 0x0c, 0x2002)
        q.write32(BASE + 0x04, 6)
        self.assertTrue(q.read32(STATUS) & 2)
        q.step(1000)
        self.assertEqual((q.read32(STATUS) >> 2) & 15, 2)
        self.assertEqual(q.read32(BASE + 0x18), 0xffffffff)
        self.assertEqual((q.read32(STATUS) >> 2) & 15, 1)
        self.assertEqual(q.read32(BASE + 0x18), 0xffffffff)
        self.assertTrue(q.read32(STATUS) & 2)
        self.assertEqual(q.read32(BASE + 0x18), 0)

        q.write32(BASE + 0x04, 6)
        q.step(1000)
        q.write32(BASE + 0x1c, 2)
        self.assertTrue(q.read32(STATUS) & 2)

    def test_secure_cpu_alias_uses_the_same_controller(self):
        from tests.test_best1700_secure_tcg import Best1700SecureTcg

        assembly = r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
vectors:
    .word 0x2415f000
    .word Reset_Handler
    .rept 142
    .word Fault_Handler
    .endr
    .org 0x400
.thumb_func
Reset_Handler:
    ldr r4, =0x44328000
    ldr r5, =0x24002000
    ldr r1, =0xe000ed08
    ldr r0, =vectors
    str r0, [r1]
    ldr r0, =0x1800
    str r0, [r4, #0x40]
1:
    ldr r0, [r4, #0x44]
    and r1, r0, #0x1e000000
    cmp r1, #0x06000000
    bne 1b
    str r0, [r5, #4]
2:
    ldr r0, [r4, #0x54]
    tst r0, #0x40
    beq 2b
    str r0, [r5, #8]
    ldr r4, =0x44320000
    ldr r0, =0x2001
    str r0, [r4, #0xc]
    ldr r0, =0x12345678
    str r0, [r4, #0x14]
    movs r0, #5
    str r0, [r4, #4]
3:
    ldr r0, [r4, #0x404]
    tst r0, #1
    bne 3b
    str r0, [r5, #12]
    movs r0, #1
    str r0, [r5]
4:
    wfi
    b 4b
.thumb_func
Fault_Handler:
    ldr r5, =0x24002000
    movs r0, #255
    str r0, [r5]
    b 4b
"""
        with tempfile.TemporaryDirectory(prefix="best1700-psram-secure-") as tmp:
            secure = Best1700SecureTcg.assemble(self, Path(tmp), "psram-secure",
                                               assembly, 0x100c0000)
            q = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"],
                machine_options=(",secure-boot=on,boot-sp=0x2415f000,boot-pc=0x100c0401"
                                 f",tee-image={secure},tee-flash-base=0x10000000"
                                 ",tee-code-offset=0xc0000,tee-entry=0x100c0401"),
                accel="tcg", extra_args=("-S", "-icount", "shift=0,sleep=off"))
            self.addCleanup(q.close)
            q.qmp("cont")
            deadline = time.monotonic() + 5
            result = (0,) * 4
            while time.monotonic() < deadline:
                result = struct.unpack("<4I", q.read(0x20002000, 16))
                if result[0]:
                    break
                time.sleep(0.01)
            q.qmp("stop")
            self.assertEqual(result[0], 1, q.diagnostic("secure PSRAM completion"))
            self.assertEqual(result[1], 0x6008020)
            self.assertEqual(result[2], 0x50)
            self.assertEqual(result[3], 0x3000402)
            self.assertEqual(q.read32(PHY + 0x44), result[1])
            self.assertEqual(q.read32(PHY + 0x54), result[2])
            self.assertEqual(q.read32(STATUS), result[3])
    def test_short_transmit_waits_and_rejected_requests_require_recovery(self):
        q = self.q
        q.write32(BASE + 0x0c, 0x2002)
        q.write32(BASE + 0x14, 0x1234)
        q.write32(BASE + 0x04, 5)
        q.step(100000)
        self.assertTrue(q.read32(STATUS) & 1)
        self.assertEqual((q.read32(STATUS) >> 7) & 15, 7,
                         "a partial command must retain its first word")
        q.write32(BASE + 0x14, 0x5678)
        q.step(1000)
        self.assertFalse(q.read32(STATUS) & 1)
        self.assertEqual((q.read32(STATUS) >> 7) & 15, 8)
        for command, length in ((1, 1), (2, 1), (0x77, 1),
                                (5, 9), (6, 9), (5, 0)):
            q.write32(BASE + 0x0c, 0x2000 | length)
            q.write32(BASE + 0x04, command)
            q.step(100000)
            self.assertTrue(q.read32(STATUS) & 1, (command, length))
            q.write32(BASE + 0x1c, 3)
            self.assertFalse(q.read32(STATUS) & 1)
        q.write32(BASE + 0x04, 2)
        q.write32(BASE + 0x04, 0xff)
        q.step(1000)
        self.assertFalse(q.read32(STATUS) & 1, "package reset must recover rejection")

    def test_frequency_status_is_read_only_and_tracks_requested_point(self):
        q = self.q
        for value, expected in ((0x200, 0), (0x101, 0x301), (0x300, 0x100)):
            q.write32(BASE + 0x200, value)
            self.assertEqual(q.read32(BASE + 0x200), expected)
        q.write32(BASE + 0x44, 0xf)
        self.assertEqual(q.read32(BASE + 0x44), 0xc)

    def test_manager_init_sleep_wakeup_and_device_reset(self):
        q = self.q
        self.assertEqual((q.read32(STATUS) >> 16) & 15, 0)
        q.write32(BASE + 0x400, 1)
        self.assertEqual((q.read32(STATUS) >> 16) & 15, 4)
        q.write32(BASE + 0x24, 9)
        self.assertEqual((q.read32(STATUS) >> 16) & 15, 2)
        q.write32(BASE + 0x24, 8)
        self.assertEqual((q.read32(STATUS) >> 16) & 15, 4)
        q.write32(BASE + 0x04, 0x10)
        q.step(1000)
        self.assertEqual((q.read32(STATUS) >> 16) & 15, 2)
        q.write32(BASE + 0x04, 0x20)
        q.step(1000)
        self.assertEqual((q.read32(STATUS) >> 16) & 15, 4)
        q.write32(BASE + 0x28, 0x2424)
        q.write32(BASE + 0x04, 0xff)
        q.step(1000)
        self.assertEqual(q.read32(BASE + 0x28), 0x2424)
        self.assertEqual(q.read32(BASE + 0x400), 1)

    def test_system_reset_cancels_timers_and_clears_controller_state(self):
        q = self.q
        q.write32(PHY + 0x40, AUTO_LP)
        q.write32(BASE + 0x400, 1)
        q.write32(BASE + 0x14, 0x1234)
        q.write32(BASE + 0x0c, 0x2001)
        q.write32(BASE + 0x04, 6)
        q.qmp("system_reset")
        q.qmp("query-status")
        q.step(10000)
        self.assertEqual(self.lp_state(), 0)
        self.assertEqual(q.read32(BASE + 0x400), 0)
        self.assertEqual(q.read32(PHY + 0x40), 0)
        self.assertEqual(q.read32(PHY + 0x54), 0)
        self.assertEqual(q.read32(STATUS), 0x3000402)
        self.assertEqual(q.read32(BASE + 0x18), 0)


if __name__ == "__main__":
    unittest.main()
