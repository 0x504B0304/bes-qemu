"""Exercise BTH hardware semaphore state and AP/BTH arbitration.

Every test owns its process and synthetic code. Repeated acquisition must
fail until a bus master releases the actual shared hardware state.
"""

import os
import struct
import time
import unittest

from tests import test_best1700_bth_qtest as bth_qtest
from tests.test_best1700_peripherals_qtest import QtestMachine


MEMSC = 0x500000C0
STATUS = 0x500000D0
RESULT = 0x23001000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BthMemscTests(unittest.TestCase):
    assemble = bth_qtest.BthTests.assemble

    def machine(self, *, tcg=False):
        machine = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=b"\xfe\xe7" + bytes(32),
            machine_options=",bth-real-boot=on,boot-sp=0x203ff000,boot-pc=0x101c0001",
            accel="tcg" if tcg else "qtest",
            extra_args=("-S", "-icount", "shift=3,sleep=off") if tcg else ())
        self.addCleanup(machine.close)
        return machine

    def test_acquisition_excludes_repeated_reads_until_explicit_release(self):
        machine = self.machine()
        self.assertEqual(machine.read32(STATUS), 0xF)
        free = 0xF
        for index in range(4):
            address = MEMSC + index * 4
            self.assertEqual(machine.read32(address), 1)
            self.assertEqual(machine.read32(address), 0)
            free &= ~(1 << index)
            # Reading status never acquires another semaphore.
            self.assertEqual(machine.read32(STATUS), free)
            self.assertEqual(machine.read32(STATUS), free)
            machine.write32(address, 0xFFFFFFFE)
            self.assertEqual(machine.read32(address), 0)
            machine.write32(address, 1)
            self.assertEqual(machine.read32(STATUS), free | (1 << index))
            self.assertEqual(machine.read32(address), 1)
        self.assertEqual(machine.read32(STATUS), 0)
        for index in range(4):
            machine.write32(MEMSC + index * 4, 1)
        self.assertEqual(machine.read32(STATUS), 0xF)

    def test_bth_sys_and_aon_banks_do_not_release_each_other(self):
        machine = self.machine()
        self.assertEqual(machine.read32(MEMSC), 1)
        self.assertEqual(machine.read32(0x400000C0), 1)
        self.assertEqual(machine.read32(MEMSC), 0)
        machine.write32(0x400000C0, 1)
        machine.write32(0x40080100, 1)
        self.assertEqual(machine.read32(MEMSC), 0)
        self.assertEqual(machine.read32(0x400000C0), 1)
        machine.write32(MEMSC, 1)
        self.assertEqual(machine.read32(0x400000C0), 0)
        self.assertEqual(machine.read32(MEMSC), 1)
        # Unrelated CMU words must remain outside the sparse device.
        for address in (0x500000BC, 0x500000D4):
            machine.write32(address, 0x12345678)
            self.assertEqual(machine.read32(address), 0x12345678)

    def test_status_is_read_only_and_reset_frees_all_locks(self):
        machine = self.machine()
        # Unsupported subword/unaligned accesses cannot consume a lock.
        machine.command(f"readb {MEMSC:#x}")
        machine.command(f"readw {MEMSC:#x}")
        machine.command(f"readl {MEMSC + 1:#x}")
        self.assertEqual(machine.read32(STATUS), 0xF)
        for index in range(4):
            self.assertEqual(machine.read32(MEMSC + index * 4), 1)
        machine.write32(STATUS, 0xFFFFFFFF)
        machine.command(f"writeb {MEMSC:#x} 1")
        machine.command(f"writew {MEMSC:#x} 1")
        self.assertEqual(machine.read32(STATUS), 0)
        machine.qmp("system_reset")
        machine.qmp("query-status")
        self.assertEqual(machine.read32(STATUS), 0xF)
        for index in range(4):
            self.assertEqual(machine.read32(MEMSC + index * 4), 1)

    def test_bth_guest_contends_with_ap_and_observes_shared_state(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x23001000
    ldr r5, =0x500000c0
    ldr r0, [r5]
    str r0, [r4, #4]
    movs r0, #0x51
    str r0, [r4]
1:  ldr r0, [r5]
    cmp r0, #0
    beq 1b
    str r0, [r4, #8]
    ldr r0, [r5]
    str r0, [r4, #12]
    ldr r0, [r5, #16]
    str r0, [r4, #16]
    movs r0, #2
    str r0, [r5]
    ldr r0, [r5]
    str r0, [r4, #20]
    movs r0, #1
    str r0, [r5]
    ldr r0, [r5, #16]
    str r0, [r4, #24]
    movs r0, #0xa7
    str r0, [r4]
2:  wfi
    b 2b
    .ltorg
""")
        machine = self.machine(tcg=True)
        machine.write(0x23004000, code)
        machine.write(0x23002000, struct.pack("<II", 0x230BFF00, 0x03004001))
        machine.write32(0x400800B8, 0x23002000)
        self.assertEqual(machine.read32(MEMSC), 1)
        machine.qmp("cont")
        machine.write32(0x400800CC, 0x300)

        def await_marker(expected):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if machine.read32(RESULT) == expected:
                    return
                time.sleep(0.01)
            self.fail(machine.diagnostic(f"missing MEMSC marker {expected:#x}"))

        await_marker(0x51)
        self.assertEqual(machine.read32(RESULT + 4), 0)
        self.assertEqual(machine.read32(STATUS), 0xE)
        # The BTH loop can only progress after another bus view releases it.
        machine.write32(MEMSC, 1)
        await_marker(0xA7)
        machine.qmp("stop")
        self.assertEqual(struct.unpack("<7I", machine.read(RESULT, 28)),
                         (0xA7, 0, 1, 0, 0xE, 0, 0xF))
        self.assertEqual(machine.read32(MEMSC), 1)
        self.assertEqual(machine.read32(MEMSC), 0)
        self.assertNotIn("Lockup", machine.stderr_path.read_text(errors="replace"))

    def test_mttcg_bth_and_ap_share_aon_memsc_atomically(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x23001000
    ldr r5, =0x40080100
    ldr r0, [r5]
    str r0, [r4, #4]
    movs r0, #0x61
    str r0, [r4]
1:  ldr r0, [r5]
    cmp r0, #0
    beq 1b
    str r0, [r4, #8]
    ldr r0, [r5]
    str r0, [r4, #12]
    movs r0, #1
    str r0, [r5]
    movs r0, #0x67
    str r0, [r4]
2:  wfi
    b 2b
    .ltorg
""")
        machine = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"], image_payload=b"\xfe\xe7" + bytes(32),
            machine_options=",bth-real-boot=on,boot-sp=0x203ff000,boot-pc=0x101c0001",
            accel="tcg,thread=multi", extra_args=("-S", "-smp", "3"))
        self.addCleanup(machine.close)
        machine.write(0x23004000, code)
        machine.write(0x23002000, struct.pack("<II", 0x230BFF00, 0x03004001))
        machine.write32(0x400800B8, 0x23002000)
        self.assertEqual(machine.read32(0x40080140), 0xffff)
        self.assertEqual(machine.read32(0x40080100), 1)
        machine.qmp("cont")
        machine.write32(0x400800CC, 0x300)

        def await_marker(expected):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if machine.read32(RESULT) == expected:
                    return
                time.sleep(0.01)
            self.fail(machine.diagnostic(f"missing AON MEMSC marker {expected:#x}"))

        await_marker(0x61)
        self.assertEqual(machine.read32(RESULT + 4), 0)
        self.assertEqual(machine.read32(0x40080140), 0xfffe)
        machine.write32(0x40080100, 1)
        await_marker(0x67)
        machine.qmp("stop")
        self.assertEqual(machine.read32(0x40080140), 0xffff)



if __name__ == "__main__":
    unittest.main()
