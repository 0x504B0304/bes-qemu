"""Check page-spy register state, secure aliases and reset without firmware.

This does not test bus security enforcement: page-spy observers are not yet
wired into the interconnect. MIBAND_BEST1700_QEMU selects the built binary.
"""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


WINDOWS = (
    0x40042000, 0x40044000, 0x40046000, 0x40048000,
    0x4004A000, 0x4004C000, 0x4004E000, 0x40050000,
    0x40055000, 0x40057000, 0x40059000, 0x4005B000,
    0x4005D000, 0x4005F000, 0x40061000, 0x40063000,
    0x40065000, 0x40067000, 0x40069000, 0x4006B000,
)
SECURE_ALIAS = 0x04000000


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700PageSpyQtest(unittest.TestCase):
    def setUp(self):
        self.q = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"])
        self.addCleanup(self.q.close)

    def test_boot_bypass_sequence_and_independent_windows(self):
        q = self.q
        # The secure boot's read/modify/write must access each real device.
        for index, base in enumerate(WINDOWS):
            self.assertEqual(q.read32(base), 0)
            q.write32(base, q.read32(base) | 8)
            q.write32(base + 8, 0x20000000 + index * 0x1000)
        for index, base in enumerate(WINDOWS):
            self.assertEqual(q.read32(base), 8)
            self.assertEqual(q.read32(base + 8),
                             0x20000000 + index * 0x1000)

    def test_configuration_masks_clear_command_and_readonly_capture(self):
        q = self.q
        for base in WINDOWS:
            q.write32(base, 0xffffffff)
            self.assertEqual(q.read32(base), 0x3df)
            # Clearing PPC IRQ preserves configuration when used as RMW.
            q.write32(base, q.read32(base) | 0x20)
            self.assertEqual(q.read32(base), 0x3df)
            q.write32(base + 4, 0xffffffff)
            self.assertEqual(q.read32(base + 4), 0xff)
            for offset, value in ((8, 0x20000003), (12, 0x2000ffff),
                                  (16, 0x60000000), (20, 0x601fffff)):
                q.write32(base + offset, value)
                self.assertEqual(q.read32(base + offset), value)
            # Guest writes must never fabricate hits or captured addresses.
            for offset in range(0x18, 0x30, 4):
                q.write32(base + offset, 0xffffffff)
                self.assertEqual(q.read32(base + offset), 0)
            q.write32(base + 4, 0xf0)
            self.assertEqual(q.read32(base + 4), 0xf0)
            self.assertEqual(q.read32(base + 16), 0x60000000)
            q.write32(base + 0x30, 0xffffffff)
            self.assertEqual(q.read32(base + 0x30), 0)

    def test_system_reset_clears_configuration_and_region_state(self):
        q = self.q
        for base in WINDOWS:
            for offset in range(0, 0x18, 4):
                q.write32(base + offset, 0xffffffff)
        q.qmp("system_reset")
        q.qmp("query-status")
        for base in WINDOWS:
            for offset in range(0, 0x30, 4):
                self.assertEqual(q.read32(base + offset), 0)

    def test_m55_secure_aliases_reach_the_same_devices(self):
        commands = [shutil.which(f"arm-none-eabi-{tool}")
                    for tool in ("as", "ld", "objcopy")]
        if not all(commands):
            self.skipTest("GNU Arm binutils are required")
        # Secure aliases are private to the AP CPU, not system/qtest memory.
        # Execute real loads/stores through them instead of probing an
        # unmapped system address which merely returns zero.
        source = """
.syntax unified
.thumb
.section .text
.word 0x2415f000
.word reset
.org 0x400
.thumb_func
reset:
"""
        for index, base in enumerate(WINDOWS):
            source += f"""
    ldr r0, ={base + SECURE_ALIAS:#x}
    ldr r1, [r0]
    orr r1, r1, #8
    str r1, [r0]
    ldr r1, ={0x20000000 + index * 0x1000:#x}
    str r1, [r0, #8]
"""
        source += """
    ldr r0, =0x24001000
    movs r1, #1
    str r1, [r0]
1:  wfi
    b 1b
"""
        with tempfile.TemporaryDirectory(prefix="best1700-page-spy-") as temp:
            path = Path(temp) / "secure"
            path.with_suffix(".s").write_text(source)
            for command in (
                [commands[0], "-mthumb", "-march=armv8-m.main", "-o",
                 str(path.with_suffix(".o")), str(path.with_suffix(".s"))],
                [commands[1], "-Ttext=0x100c0000", "-e", "0x100c0401",
                 "-o", str(path.with_suffix(".elf")), str(path.with_suffix(".o"))],
                [commands[2], "-O", "binary", str(path.with_suffix(".elf")),
                 str(path.with_suffix(".bin"))],
            ):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
            q = QtestMachine(
                os.environ["MIBAND_BEST1700_QEMU"], accel="tcg",
                machine_options=(",secure-boot=on,boot-sp=0x2415f000"
                                 ",boot-pc=0x100c0401"
                                 f",tee-image={path.with_suffix('.bin')}"
                                 ",tee-flash-base=0x10000000"
                                 ",tee-code-offset=0xc0000,tee-entry=0x100c0401"),
                extra_args=("-S", "-icount", "shift=0,sleep=off"))
            try:
                q.qmp("cont")
                deadline = time.monotonic() + 5
                while q.read32(0x20001000) != 1 and time.monotonic() < deadline:
                    time.sleep(0.01)
                q.qmp("stop")
                self.assertEqual(q.read32(0x20001000), 1,
                                 q.diagnostic("M55 secure page-spy access"))
                for index, base in enumerate(WINDOWS):
                    self.assertEqual(q.read32(base), 8)
                    self.assertEqual(q.read32(base + 8),
                                     0x20000000 + index * 0x1000)
            finally:
                q.close()


if __name__ == "__main__":
    unittest.main()
