"""Exercise the BEST1700 BTC CPU trace RAM and control commands."""

import os
import struct
import subprocess
import unittest

from tests import test_best1700_btc_machine_qtest as btc_qtest


BASE = 0x6E100000
ENABLE = BASE + 0x2000
COMMAND = BASE + 0x2004
BTC_AON = 0x40080000
BTC_VTOR = BTC_AON + 0x168
BTC_RESET_RELEASE = BTC_AON + 0xCC


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcCpudumpTests(unittest.TestCase):
    machine = btc_qtest.BtcMachineTests.machine

    def test_control_registers_and_trace_ram_are_diagnostic(self):
        machine = self.machine()
        self.assertEqual(machine.read32(ENABLE), 0)
        machine.write32(ENABLE, 1)
        self.assertEqual(machine.read32(ENABLE), 1)
        for value in (0, 1, 2, 0, 1, 0, 2):
            machine.write32(COMMAND, value)
            self.assertEqual(machine.read32(ENABLE), 1)
            self.assertEqual(machine.read32(COMMAND), 0)
            self.assertEqual(machine.read32(COMMAND), 0)

        machine.write32(ENABLE, 3)
        machine.command(f"writeb {ENABLE:#x} 0")
        self.assertEqual(machine.read32(ENABLE), 1)
        machine.write32(COMMAND, 3)
        machine.write32(BASE, 0x60401235)
        self.assertEqual(machine.read32(BASE), 0x60401235)
        self.assertEqual(machine.read32(COMMAND), 0)

        log = machine.stderr_path.read_text(errors="replace")
        for text in (
            "CPUDUMP unsupported write offset=0x2000 value=0x3",
            "CPUDUMP unsupported write offset=0x2004 value=0x3",
        ):
            self.assertIn(text, log)
        self.assertNotIn("CPUDUMP unsupported read offset=0x2004", log)
        self.assertNotIn("CPUDUMP unsupported read offset=0x0", log)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        self.assertEqual(machine.read32(ENABLE), 0)
        self.assertEqual(machine.read32(COMMAND), 0)
        self.assertEqual(machine.read32(BASE), 0)

    def test_command_readback_does_not_ack_irqs_or_open_trace_neighbors(self):
        machine = self.machine()
        machine.command("irq_intercept_in /machine/btc-core")
        machine.write32(0x6E030024, 4)
        machine.write32(0x6E030048, 1)
        machine.write32(ENABLE, 1)
        machine.write32(COMMAND, 2)
        for _ in range(3):
            self.assertEqual(machine.read32(COMMAND), 0)
        machine.step(31_249)
        self.assertFalse(machine.irq(6))
        machine.step(1)
        self.assertTrue(machine.irq(6))
        for value in (0, 1, 2, 0, 1, 2):
            machine.write32(COMMAND, value)
            self.assertEqual(machine.read32(COMMAND), 0)
            self.assertTrue(machine.irq(6))

        machine.write32(BASE, 0x12345678)
        machine.write32(BASE + 0x1FFC, 0x87654321)
        self.assertEqual(machine.read32(BASE), 0x12345678)
        self.assertEqual(machine.read32(BASE + 0x1FFC), 0x87654321)
        machine.write32(BASE + 0x2008, 0)
        self.assertEqual(machine.read32(BASE + 0x2008), 0)
        self.assertTrue(machine.irq(6))
        self.assertEqual(machine.read32(ENABLE), 1)
        self.assertEqual(machine.read32(0x6E030048), 0x800005DC)
        self.assertFalse(machine.irq(6))

        log = machine.stderr_path.read_text(errors="replace")
        for operation in ("read", "write"):
            self.assertIn(
                f"CPUDUMP unsupported {operation} offset=0x2008", log)
        for value in (3, 4, 0x80000000, 0xFFFFFFFF):
            machine.write32(COMMAND, value)
            self.assertIn(
                f"CPUDUMP unsupported write offset=0x2004 value={value:#x}",
                machine.stderr_path.read_text(errors="replace"))

    def test_configured_period_stop_restart_reset_and_startup_guard(self):
        machine = btc_qtest.QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
            machine_options=',bth-real-boot=on,btc-real-boot=on,btc-cpudump-sample-ns=1000000',
            extra_args=('-smp', '4'))
        self.addCleanup(machine.close)
        prop = dict(path='/machine', property='btc-cpudump-sample-ns')
        self.assertEqual(machine.qmp('qom-get', prop), 1000000)
        with self.assertRaisesRegex(RuntimeError, 'startup-only'):
            machine.qmp('qom-set', dict(prop, value=1000))
        machine.write(0x68000000, struct.pack('<II', 0x68017f00, 0x68000101))
        machine.write(0x68000100, bytes.fromhex('30bf fee7'))
        machine.write32(BTC_VTOR, 0x68000000)
        machine.write32(BTC_RESET_RELEASE, 0x88)
        machine.write32(ENABLE, 1)
        machine.write32(COMMAND, 2)
        machine.step(999999)
        self.assertEqual(machine.read32(BASE), 0)
        machine.step(1)
        self.assertEqual(machine.read32(BASE), 0x68000101)
        self.assertEqual(machine.read32(BASE + 4), 0)
        machine.write32(COMMAND, 1)
        machine.step(10000000)
        self.assertEqual(machine.read32(BASE + 4), 0)
        machine.write32(COMMAND, 2)
        machine.step(1000000)
        self.assertEqual(machine.read32(BASE + 4), 0x68000101)
        machine.qmp('system_reset')
        machine.qmp('query-status')
        self.assertEqual(machine.read32(ENABLE), 0)
        self.assertEqual(machine.read32(BASE), 0)
        self.assertEqual(machine.qmp('qom-get', prop), 1000000)
        for value in (0, 1000000001):
            result = subprocess.run([os.environ['MIBAND_BEST1700_QEMU'], '-M',
                f'miband-best1700,btc-cpudump-sample-ns={value}', '-display', 'none'],
                capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('btc-cpudump-sample-ns must be between', result.stderr)

    def test_capture_samples_btc_pc_after_reset_release_and_clears_on_reset(self):
        machine = self.machine()
        vector = struct.pack("<II", 0x68017F00, 0x68000101)
        machine.write(0x68000000, vector)
        machine.write(0x68000100, bytes.fromhex("30bf fee7"))
        machine.write32(BTC_VTOR, 0x68000000)
        machine.write32(BTC_RESET_RELEASE, 0x88)
        machine.write32(ENABLE, 1)
        machine.write32(COMMAND, 2)
        machine.step(10_000)

        self.assertEqual(machine.read32(BASE), 0x68000101)
        self.assertEqual(machine.read32(BASE + 4), 0x68000101)
        self.assertEqual(machine.read32(ENABLE), 1)

        machine.write32(COMMAND, 1)
        machine.step(5_000)
        self.assertEqual(machine.read32(BASE), 0x68000101)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        self.assertEqual(machine.read32(ENABLE), 0)
        self.assertEqual(machine.read32(COMMAND), 0)
        self.assertEqual(machine.read32(BASE), 0)


if __name__ == "__main__":
    unittest.main()
