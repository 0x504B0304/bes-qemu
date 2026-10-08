"""Exercise shared modem configuration without synthesizing radio results."""
import os
import struct
import time
import unittest

from tests import test_best1700_bth_qtest as bth_qtest
from tests.test_best1700_peripherals_qtest import QtestMachine


MODEM = 0x6E050000
AON = 0x40080000
RESULT = 0x68001000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcModemTests(unittest.TestCase):
    assemble = bth_qtest.BthTests.assemble

    def machine(self, *, tcg=False):
        machine = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=b"\xfe\xe7" + bytes(32),
            machine_options=",bth-real-boot=on,btc-real-boot=on",
            accel="tcg" if tcg else "qtest",
            extra_args=["-smp", "4", "-S", "-d", "unimp",
                        "-icount", "shift=3,sleep=off"] if tcg else
                       ["-smp", "4", "-d", "unimp"])
        self.addCleanup(machine.close)
        return machine

    def test_configuration_is_shared_with_executing_btc_and_machine_reset(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset
.thumb_func
reset:
    ldr r0, =0x6e050000
    ldr r1, =0x68001000
    ldr r2, [r0, #0x15c]
    str r2, [r1]
    ldr r2, [r0, #0x1bc]
    str r2, [r1, #4]
    movs r2, #2
    str r2, [r0, #0x15c]
    movs r2, #1
    str r2, [r1, #8]
1:  wfi
    b 1b
    .ltorg
""", base=0x68004000)
        machine = self.machine(tcg=True)
        machine.write32(MODEM + 0x15C, 1)
        machine.write32(MODEM + 0x1BC, 0x30000000)
        machine.write32(MODEM + 0x228, 0x60000000)
        self.assertEqual(machine.read32(MODEM + 0x228), 0x60000000)
        machine.write(0x68004000, code)
        machine.write(0x68002000, struct.pack("<II", 0x68017F00, 0x68004001))
        machine.write32(AON + 0x168, 0x68002000)
        machine.qmp("cont")
        machine.write32(AON + 0xCC, 0x88)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and machine.read32(RESULT + 8) != 1:
            time.sleep(0.01)
        machine.qmp("stop")
        self.assertEqual(machine.read32(RESULT + 8), 1,
                         machine.diagnostic("BTC did not finish modem access"))
        self.assertEqual(machine.read32(RESULT), 1)
        self.assertEqual(machine.read32(RESULT + 4), 0x30000000)
        self.assertEqual(machine.read32(MODEM + 0x15C), 2)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        self.assertEqual(machine.read32(MODEM + 0x15C), 0)
        self.assertEqual(machine.read32(MODEM + 0x1BC), 0)
        self.assertEqual(machine.read32(MODEM + 0x228), 0)

    def test_unsupported_fields_and_access_width_do_not_change_configuration(self):
        machine = self.machine()
        machine.write32(MODEM + 0x028, 0x12345678)
        machine.write32(MODEM + 0x02C, 0x0b)
        machine.write32(MODEM + 0x250, 0xa1234567)
        self.assertEqual(machine.read32(MODEM + 0x028), 0x12345678)
        self.assertEqual(machine.read32(MODEM + 0x02C), 0x0b)
        self.assertEqual(machine.read32(MODEM + 0x250), 0xa1234567)
        machine.write32(MODEM + 0x15C, 1)
        machine.write32(MODEM + 0x15C, 0x80000002)
        machine.command(f"writeb {MODEM + 0x15C:#x} 2")
        machine.command(f"writew {MODEM + 0x15C:#x} 2")
        self.assertEqual(machine.read32(MODEM + 0x15C), 1)
        self.assertIn("BTC modem unsupported write offset=0x15c",
                      machine.stderr_path.read_text(errors="replace"))

    def test_ble_tx_power_coefficients_are_three_packed_10bit_fields(self):
        machine = self.machine()
        values = {
            0x0C4: 0x000003FF | (0x155 << 10) | (0x2AA << 20),
            0x0C8: 0x00000001 | (0x2AB << 10) | (0x3FE << 20),
            0x0CC: 0x000003FF | (0x000 << 10),
        }
        for offset, value in values.items():
            machine.write32(MODEM + offset, value)
            self.assertEqual(machine.read32(MODEM + offset), value)
            machine.write32(MODEM + offset, value | 0xC0000000)
            self.assertEqual(machine.read32(MODEM + offset), value)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        for offset in values:
            self.assertEqual(machine.read32(MODEM + offset), 0)

    def test_mixer_command_and_energy_readbacks_follow_register_semantics(self):
        machine = self.machine()
        # The firmware writes mixer setup fields together with START_MIX.  Both
        # phases must be accepted; with no RF source attached the readbacks
        # remain the reset/no-signal value.
        for command in (0x9E000168, 0x9E800168, 0xDE000168, 0xDE800168):
            machine.write32(MODEM + 0x334, command)
            self.assertEqual(machine.read32(MODEM + 0x334), 0)
            self.assertEqual(machine.read32(MODEM + 0x338), 0)
            self.assertEqual(machine.read32(MODEM + 0x33C), 0)
        machine.write32(MODEM + 0x334, 1 << 30)
        self.assertEqual(machine.read32(MODEM + 0x334), 0)
        self.assertEqual(machine.read32(MODEM + 0x338), 0)
        self.assertEqual(machine.read32(MODEM + 0x33C), 0)
        machine.write32(MODEM + 0x334, 0xDE800169)
        log = machine.stderr_path.read_text(errors="replace")
        self.assertIn("unsupported write offset=0x334", log)

    def test_energy_readbacks_are_read_only(self):
        machine = self.machine()
        for offset in (0x338, 0x33C):
            machine.write32(MODEM + offset, 0x123456)
            self.assertEqual(machine.read32(MODEM + offset), 0)
        log = machine.stderr_path.read_text(errors="replace")
        self.assertIn("unsupported write offset=0x338", log)
        self.assertIn("unsupported write offset=0x33c", log)


if __name__ == "__main__":
    unittest.main()
