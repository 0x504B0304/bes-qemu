"""Check the optional BTC processor's machine wiring with synthetic guests."""

import json
import os
import struct
import subprocess
import time
import unittest

from tests import test_best1700_bth_qtest as bth_qtest
from tests.test_best1700_peripherals_qtest import QtestMachine


AON = 0x40080000
UART = 0x6E000000
RESULT = 0x68001000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcMachineTests(unittest.TestCase):
    assemble = bth_qtest.BthTests.assemble

    def machine(self, *, btc=True, tcg=False, uart=False):
        options = ",boot-sp=0x203ff000,boot-pc=0x101c0001"
        args = ["-d", "unimp"]
        if btc:
            options += ",bth-real-boot=on,btc-real-boot=on"
            args += ["-smp", "4"]
        if tcg:
            args += ["-S", "-icount", "shift=3,sleep=off"]
        if uart:
            # QtestMachine appends three file backends after these two:
            # its last serial file therefore captures controller serial 4.
            args += ["-serial", "null", "-serial", "null"]
        machine = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=b"\xfe\xe7" + bytes(32), machine_options=options,
            accel="tcg" if tcg else "qtest", extra_args=args)
        self.addCleanup(machine.close)
        return machine

    def await_word(self, machine, address, value):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if machine.read32(address) == value:
                return
            time.sleep(0.01)
        self.fail(machine.diagnostic(f"missing word {value:#x} at {address:#x}"))

    def test_smp_configuration_must_match_controller_mode(self):
        for options, count in (("", "4"),
                               (",bth-real-boot=on,btc-real-boot=on", "3")):
            with self.subTest(options=options, count=count):
                result = subprocess.run([
                    os.environ["MIBAND_BEST1700_QEMU"], "-M",
                    "miband-best1700" + options, "-smp", count,
                    "-accel", "qtest", "-display", "none", "-monitor", "none",
                ], capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("-smp", result.stderr)

    def test_qmp_cpu_inventory_matches_enabled_controller(self):
        for enabled in (False, True):
            with self.subTest(btc=enabled):
                machine = self.machine(btc=enabled)
                cpus = machine.qmp("query-cpus-fast")
                self.assertEqual([cpu["cpu-index"] for cpu in cpus],
                                 list(range(4 if enabled else 3)))
                paths = {cpu["qom-path"] for cpu in cpus}
                self.assertEqual("/machine/btc-core/cpu" in paths, enabled)
                status = json.loads(machine.qmp("qom-get", {
                    "path": "/machine", "property": "core-status"}))
                self.assertEqual(status["btc"], "held-reset" if enabled else "disabled")

    def test_calibration_clocks_reject_invalid_ranges_and_runtime_changes(self):
        for option in ('btc-lpo-hz=0', 'btc-lpo-hz=1000001',
                       'btc-cal-ref-hz=0', 'btc-cal-ref-hz=1000000001',
                       'btc-cal-ref-hz=1,btc-lpo-hz=2', 'btc-lpo-hz=1'):
            with self.subTest(option=option):
                result = subprocess.run([
                    os.environ['MIBAND_BEST1700_QEMU'], '-M',
                    'miband-best1700,' + option, '-accel', 'qtest',
                    '-display', 'none', '-monitor', 'none',
                ], capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(result.stderr, 'frequency range|clock ratio')
        machine = self.machine()
        for prop, expected in (('btc-cal-ref-hz', 48000000), ('btc-lpo-hz', 32000)):
            args = {'path': '/machine', 'property': prop}
            self.assertEqual(machine.qmp('qom-get', args), expected)
            with self.assertRaisesRegex(RuntimeError, 'startup-only'):
                machine.qmp('qom-set', dict(args, value=12345))
            self.assertEqual(machine.qmp('qom-get', args), expected)

    def test_system_reset_clears_uart_and_preserves_host_connection(self):
        machine = self.machine(uart=True)
        machine.write32(UART + 0x30, 0x101)
        machine.write32(UART + 0x24, 0x1234)
        machine.write32(UART, ord("A"))
        self.assertEqual(machine.serial(2), b"A")
        machine.qmp("system_reset")
        machine.qmp("query-status")
        self.assertEqual(machine.read32(UART + 0x30), 0)
        self.assertEqual(machine.read32(UART + 0x24), 0)
        machine.write32(UART, ord("B"))
        self.assertEqual(machine.serial(2), b"A")
        machine.write32(UART + 0x30, 0x101)
        machine.write32(UART, ord("C"))
        self.assertEqual(machine.serial(2), b"AC")

    def test_data_alias_updates_previously_translated_btc_instructions(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x68001000
    ldr r6, =0x60406001
    blx r6
    str r0, [r4]
1:  ldr r0, [r4, #4]
    cmp r0, #1
    bne 1b
    blx r6
    str r0, [r4, #8]
2:  wfi
    b 2b
    .ltorg
""", base=0x68004000)
        machine = self.machine(tcg=True)
        machine.write32(AON + 0x98, 1)
        machine.write(0x68004000, code)
        machine.write32(0x68406000, 0x47702011)  # movs r0, #0x11; bx lr
        machine.write(0x68002000, struct.pack("<II", 0x68017F00, 0x68004001))
        machine.write32(AON + 0x168, 0x68002000)
        machine.qmp("cont")
        machine.write32(AON + 0xCC, 0x88)
        self.await_word(machine, RESULT, 0x11)
        # No reset or ownership transition occurs between the two calls.
        machine.write32(0x68406000, 0x47702022)
        machine.write32(RESULT + 4, 1)
        self.await_word(machine, RESULT + 8, 0x22)
        machine.qmp("stop")
        self.assertNotIn("Lockup", machine.stderr_path.read_text(errors="replace"))


if __name__ == "__main__":
    unittest.main()
