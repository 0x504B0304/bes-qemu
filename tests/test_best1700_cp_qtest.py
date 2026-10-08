"""Check CP reset/wait ownership and physical memory aliases with synthetic RAM.

Each test owns its QEMU process. These tests neither load original firmware
nor connect to an existing emulator session.
"""
import json
import os
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


CMU = 0x40080000
VTOR = CMU + 0xb4
PULSE = CMU + 0xc4
RESET_SET = CMU + 0xc8
RESET_CLEAR = CMU + 0xcc
CORE_WAIT = CMU + 0x1a4
CP_RESET = 1 << 6
CP_WAIT = 1 << 1


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class CpRegisterTests(unittest.TestCase):
    def machine(self, *, real=True, bth=False):
        options = ',cp-real-boot=on' if real else ''
        if bth:
            options += ',bth-real-boot=on'
        machine = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                                machine_options=options)
        self.addCleanup(machine.close)
        return machine

    def test_default_cp_mode_preserves_stub_entry(self):
        machine = self.machine(real=False)
        status = json.loads(machine.qmp('qom-get', {
            'path': '/machine', 'property': 'core-status'}))
        self.assertEqual(status['cp'], 'stub')

    def test_reset_release_and_wait_have_independent_latches(self):
        machine = self.machine()
        self.assertEqual(machine.read32(RESET_SET) & CP_RESET, 0)
        self.assertEqual(machine.read32(RESET_CLEAR) & CP_RESET, 0)
        self.assertEqual(machine.read32(CORE_WAIT) & CP_WAIT, CP_WAIT)
        machine.write32(RESET_CLEAR, CP_RESET)
        self.assertEqual(machine.read32(RESET_SET) & CP_RESET, CP_RESET)
        self.assertEqual(machine.read32(CORE_WAIT) & CP_WAIT, CP_WAIT)
        machine.write32(CORE_WAIT, 0)
        self.assertEqual(machine.read32(CORE_WAIT) & CP_WAIT, 0)
        self.assertEqual(machine.read32(RESET_CLEAR) & CP_RESET, CP_RESET)
        machine.write32(CORE_WAIT, CP_WAIT)
        self.assertEqual(machine.read32(RESET_CLEAR) & CP_RESET, CP_RESET)
        machine.write32(RESET_SET, CP_RESET)
        self.assertEqual(machine.read32(RESET_CLEAR) & CP_RESET, 0)
        self.assertEqual(machine.read32(CORE_WAIT) & CP_WAIT, CP_WAIT)
        machine.write32(PULSE, CP_RESET)
        self.assertEqual(machine.read32(RESET_CLEAR) & CP_RESET, 0)
        self.assertEqual(machine.read32(PULSE) & CP_RESET, 0)

    def test_vector_alignment_and_partial_writes(self):
        machine = self.machine()
        machine.write32(VTOR, 0x200dff7f)
        self.assertEqual(machine.read32(VTOR) & ~127, 0x200dff00)
        machine.command(f'writeb {VTOR + 1:#x} 0xaa')
        self.assertEqual(machine.read32(VTOR) & ~127, 0x200daa00)
        machine.command(f'writeb {VTOR:#x} 0xff')
        self.assertEqual(machine.read32(VTOR) & ~127, 0x200daa80)
        machine.command(f'writew {VTOR + 2:#x} 0x2011')
        self.assertEqual(machine.read32(VTOR) & ~127, 0x2011aa80)

    def test_cp_commands_cannot_change_bth_reset_and_respect_byte_lanes(self):
        machine = self.machine(bth=True)
        machine.write32(RESET_CLEAR, CP_RESET)
        self.assertEqual(machine.read32(RESET_CLEAR) & 0x340, CP_RESET)
        machine.write32(RESET_CLEAR, 0x300)
        self.assertEqual(machine.read32(RESET_CLEAR) & 0x340, 0x340)
        machine.write32(RESET_SET, CP_RESET)
        self.assertEqual(machine.read32(RESET_CLEAR) & 0x340, 0x300)
        machine.command(f'writeb {RESET_CLEAR + 1:#x} 0x40')
        self.assertEqual(machine.read32(RESET_CLEAR) & CP_RESET, 0)
        machine.command(f'writeb {RESET_CLEAR:#x} 0x40')
        self.assertEqual(machine.read32(RESET_CLEAR) & CP_RESET, CP_RESET)
        machine.command(f'writeb {CORE_WAIT + 1:#x} 0')
        self.assertEqual(machine.read32(CORE_WAIT) & CP_WAIT, CP_WAIT)
        machine.command(f'writeb {CORE_WAIT:#x} 0')
        self.assertEqual(machine.read32(CORE_WAIT) & CP_WAIT, 0)

    def test_cp_axi_tcm_storage_is_separate_from_ap_local_tcm(self):
        machine = self.machine()
        pairs = [(0x00100400, 0x20f00400), (0x20000440, 0x21c00440),
                 (0x200dff00, 0x21cdff00)]
        for index, (ap, cp_axi) in enumerate(pairs):
            with self.subTest(ap=hex(ap), cp_axi=hex(cp_axi)):
                machine.write32(ap, 0xa0000000 | index)
                machine.write32(cp_axi, 0xc0000000 | index)
                self.assertEqual(machine.read32(ap), 0xa0000000 | index)
                self.assertEqual(machine.read32(cp_axi), 0xc0000000 | index)
        machine.write32(0x1b300200, 0x11223344)
        self.assertEqual(machine.read32(0x3b300200), 0x11223344)
        machine.write32(0x3b300204, 0x55667788)
        self.assertEqual(machine.read32(0x1b300204), 0x55667788)

    def test_system_reset_reasserts_cp_and_restores_wait(self):
        machine = self.machine()
        machine.write32(VTOR, 0x200dff00)
        machine.write32(RESET_CLEAR, CP_RESET)
        machine.write32(CORE_WAIT, 0)
        machine.qmp('system_reset')
        machine.qmp('query-status')
        self.assertEqual(machine.read32(VTOR) & ~127, 0)
        self.assertEqual(machine.read32(RESET_CLEAR) & CP_RESET, 0)
        self.assertEqual(machine.read32(CORE_WAIT) & CP_WAIT, CP_WAIT)


if __name__ == '__main__':
    unittest.main()
