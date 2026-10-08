"""Exercise BTC baseband configuration, sampled clocks and target-3 deadlines.

Synthetic guest code uses the physical BTC bus; qtest observes shared state
through the machine bus. No test supplies an HCI response or radio event.
"""

import os
import struct
import time
import unittest

from tests import test_best1700_bth_qtest as bth_qtest
from tests.test_best1700_peripherals_qtest import QtestMachine


BASEBAND = 0x6D040000
TARGET3 = 0x80
# Inactive words replayed by the common, BLE and BT context restore routines.
ZERO_RESTORE_OFFSETS = (
    0xE4, 0x194, 0x474, 0x600, 0x604, 0x82C,
    0x934, 0x990, 0x9A0, 0x9A4, 0x9A8, 0x9AC, 0x9B0, 0x9B4,
    0x9F0, 0x9F4, 0x9F8, 0x9FC, 0xA00, 0xA04, 0xA08, 0xA0C,
    0xC30, 0xC34, 0xC40, 0xC44, 0xC48, 0xC78,
    0xCD0, 0xCD4,
    0xCD8, 0xCDC, 0xD2C, 0xD30, 0xD34, 0xD38, 0xD3C,
    0xD40, 0xD44, 0xD48, 0xD4C, 0xD50, 0xD58, 0xD5C,
)


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class BtcBasebandTests(unittest.TestCase):
    assemble = bth_qtest.BthTests.assemble

    def machine(self, *, tcg=False):
        args = ["-smp", "4", "-d", "unimp"]
        if tcg:
            args += ["-S", "-icount", "shift=3,sleep=off"]
        q = QtestMachine(
            os.environ["MIBAND_BEST1700_QEMU"],
            image_payload=b"\xfe\xe7" + bytes(32),
            machine_options=(",cp-real-boot=on,bth-real-boot=on,btc-real-boot=on"
                             ",boot-sp=0x203ff000,boot-pc=0x101c0001"),
            accel="tcg" if tcg else "qtest", extra_args=args)
        self.addCleanup(q.close)
        return q

    def test_ip_revisions_and_idle_local_reset_preserve_common_deadline(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(0x6D020100, 0xC0DEC0DE)
        q.write32(BASEBAND + 0xC74, 0x12345678)
        q.write32(BASEBAND + 0x18, TARGET3)
        q.write32(BASEBAND + 0xF8, 4)
        q.write32(BASEBAND + 0xFC, 624)
        q.write32(BASEBAND + 0x400, 0x80E0D)
        q.write32(BASEBAND + 0x800, 0x100607)
        for control, revision, version in ((0x400, 0x404, 0x0B000600),
                                            (0x800, 0x804, 0x0B001200)):
            self.assertEqual(q.read32(BASEBAND + revision), version)
            q.write32(BASEBAND + revision, 0xFFFFFFFF)
            self.assertEqual(q.read32(BASEBAND + revision), version)
            before = q.read32(BASEBAND + control)
            q.write32(BASEBAND + control, before | 0x80000000)
            self.assertEqual(q.read32(BASEBAND + control), before)
        q.command("clock_step 1250000")
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        self.assertTrue(q.irq(1))
        self.assertEqual(q.read32(BASEBAND + 0x410), 0)
        self.assertEqual(q.read32(BASEBAND + 0x810), 0)
        # BTC warm restore polls the idle MAC status word at +0x110.
        # It is a reset-value read and must not inject a guest BusFault.
        self.assertEqual(q.read32(BASEBAND + 0x110), 0)
        q.write32(BASEBAND + 0x110, 1 << 31)
        self.assertEqual(q.read32(BASEBAND + 0x110), 0)
        self.assertEqual(q.read32(0x6D020100), 0xC0DEC0DE)
        self.assertEqual(q.read32(BASEBAND + 0xC74), 0x12345678)
        for control in (0x400, 0x800):
            before = q.read32(BASEBAND + control) | 0x100
            q.write32(BASEBAND + control, before)
            q.write32(BASEBAND + control, before | 0x80000000)
            self.assertEqual(q.read32(BASEBAND + control), before)
        for offset in (0x610, 0x630, 0x650, 0x680, 0x8D0):
            self.assertEqual(q.read32(BASEBAND + offset), 0)
            q.write32(BASEBAND + offset, 0)
            q.write32(BASEBAND + offset, 1)
            self.assertEqual(q.read32(BASEBAND + offset), 0)

    def test_software_request_latches_under_mask_and_guest_ack_clears(self):
        q = self.machine()
        q.command('irq_intercept_in /machine/btc-core')
        q.write32(BASEBAND, 1 << 27)
        self.assertEqual(q.read32(BASEBAND), 0)
        self.assertEqual(q.read32(BASEBAND + 0x1c), 0)
        self.assertFalse(q.irq(1))
        q.write32(BASEBAND + 0x18, 8)
        self.assertEqual(q.read32(BASEBAND + 0x1c), 8)
        self.assertTrue(q.irq(1))
        q.write32(BASEBAND, 0)
        q.write32(BASEBAND, 1 << 27)
        q.write32(BASEBAND + 0x20, 4)
        self.assertEqual(q.read32(BASEBAND + 0x1c), 8)
        q.write32(BASEBAND + 0x20, 8)
        self.assertFalse(q.irq(1))
        q.step(100_000_000)
        self.assertEqual(q.read32(BASEBAND + 0x1c), 0)
        # Unknown commands reject the whole transaction, including SWINT.
        q.write32(BASEBAND, (1 << 27) | (1 << 26))
        self.assertEqual(q.read32(BASEBAND + 0x1c), 0)
        q.write32(BASEBAND, 1 << 27)
        q.write32(BASEBAND, 1 << 31)
        self.assertEqual(q.read32(BASEBAND + 0x1c), 0)
        self.assertFalse(q.irq(1))

    def test_idle_ble_abort_consumes_requests_without_radio_events(self):
        q = self.machine()
        q.command('irq_intercept_in /machine/btc-core')
        for i in range(16):
            q.write32(0x6d020000 + i * 16, 0x18)
        q.write32(0x6d021220, 0x12348001)  # RAL belongs to the guest.
        config, advert, scan = 0x100707, 1 << 25, 1 << 24
        q.write32(BASEBAND + 0x800, config)
        q.write32(BASEBAND + 0x18, 0x80ff)
        q.write32(BASEBAND + 0xf8, 4)
        q.write32(BASEBAND + 0xfc, 624)
        q.write32(BASEBAND + 0x800, config | advert)
        self.assertEqual(q.read32(BASEBAND + 0x800), config | advert)
        q.write32(BASEBAND + 0x800, q.read32(BASEBAND + 0x800) | scan)
        q.write32(BASEBAND + 0x800, config)  # Zero isn't a command cancellation.
        q.step(499)
        self.assertEqual(q.read32(BASEBAND + 0x800), config | advert | scan)
        q.step(1)
        self.assertEqual(q.read32(BASEBAND + 0x800), config)
        self.assertEqual(q.read32(BASEBAND + 0x810), 0)
        self.assertFalse(q.irq(1))
        self.assertEqual(q.read32(0x6d021220), 0x12348001)
        for i in range(16):
            self.assertEqual(q.read32(0x6d020000 + i * 16), 0x18)
        q.step(1_249_500)
        self.assertEqual(q.read32(BASEBAND + 0x1c), TARGET3,
                         'abort must preserve independent clock deadlines')
        for command in (advert, scan):
            q.write32(BASEBAND + 0x800, config | command)
            q.step(500)
            self.assertEqual(q.read32(BASEBAND + 0x800), config)

    def test_ble_abort_checks_entire_selected_table_and_rejects_active_state(self):
        q = self.machine()
        config, commands = 0x100707, (1 << 25) | (1 << 24)
        q.write32(BASEBAND + 0x800, config)
        # Uninitialized, scheduled and active states must not be accepted as idle.
        for state in (0, 1, 2, 4, 5, 6, 7):
            for i in range(16):
                q.write32(0x6d020000 + i * 16, 0x18)
            q.write32(0x6d0200f0, state << 3)
            q.write32(BASEBAND + 0x800, (config ^ 1) | commands)
            self.assertEqual(q.read32(BASEBAND + 0x800), config)
            q.step(500)
            self.assertEqual(q.read32(BASEBAND + 0x800), config)
        # ET pointer uses halfword units, not bytes or a fixed firmware offset.
        for i in range(16):
            q.write32(0x6d020200 + i * 16, 0x18)
        q.write32(BASEBAND + 0x2c, 0x100)
        q.write32(BASEBAND + 0x800, config | commands | (1 << 26))
        self.assertEqual(q.read32(BASEBAND + 0x800), config)
        q.write32(BASEBAND + 0x800, config | commands)
        q.step(500)
        self.assertEqual(q.read32(BASEBAND + 0x800), config)
        q.write32(BASEBAND + 0x800, config | commands)
        q.write32(0x6d020200, 1 << 3)
        q.step(500)
        self.assertEqual(q.read32(BASEBAND + 0x800), config | commands,
                         'new activity cannot receive invented abort completion')
        q.write32(BASEBAND, 1 << 31)
        self.assertEqual(q.read32(BASEBAND + 0x800), config)
        q.step(500)
        self.assertEqual(q.read32(BASEBAND + 0x810), 0)
        q.write32(0x6d020200, 0x18)
        q.write32(BASEBAND + 0x800, config | commands)
        q.qmp('system_reset')
        q.qmp('query-status')
        q.step(500)
        self.assertEqual(q.read32(BASEBAND + 0x800), 0)

    def test_live_iso_counter_fraction_wrap_and_sample_independence(self):
        q = self.machine()
        live = BASEBAND + 0xe78
        self.assertEqual(q.read32(live), 0)
        q.step(999)
        self.assertEqual(q.read32(live), 0)
        q.step(1)
        self.assertEqual(q.read32(live), 1)
        q.write32(BASEBAND + 0x100, 1 << 31)
        self.assertEqual(q.read32(BASEBAND + 0x9c4), 1)
        q.step(2500)
        self.assertEqual(q.read32(live), 3)
        self.assertEqual(q.read32(BASEBAND + 0x9c4), 1)
        q.write32(live, 0xffffffff)
        self.assertEqual(q.read32(live), 3, 'read-only counter rejects writes')
        q.step((1 << 32) * 1000 - 3500)
        self.assertEqual(q.read32(live), 0)
        q.step(1000)
        self.assertEqual(q.read32(live), 1)
        q.write32(BASEBAND, 1 << 31)
        self.assertEqual(q.read32(live), 1, 'MAC reset preserves common clocks')
        q.qmp('system_reset')
        self.assertEqual(q.read32(live), 0)

    def test_bt_extension_interrupt_masks_do_not_generate_status(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        mask = 0x10016
        for bit in (17, 18, 19, 20, 22, 23, 24):
            mask |= 1 << bit
            q.write32(BASEBAND + 0x40C, mask)
            self.assertEqual(q.read32(BASEBAND + 0x40C), mask)
        q.command("clock_step 10000000")
        self.assertEqual(q.read32(BASEBAND + 0x410), 0)
        self.assertFalse(q.irq(0))
        q.write32(BASEBAND + 0x40C, mask | (1 << 21))
        self.assertEqual(q.read32(BASEBAND + 0x40C), mask)
        q.write32(BASEBAND + 0x400, 0x80000000)
        self.assertEqual(q.read32(BASEBAND + 0x40C), mask)

    def test_timestamp_targets_expire_independently_and_ack_does_not_rearm(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0xE0)
        for index in range(3):
            q.write32(BASEBAND + 0xE8 + index * 8, index + 1)
            q.write32(BASEBAND + 0xEC + index * 8, 624)
        for index in range(3):
            q.step(312_499)
            self.assertFalse(q.irq(1))
            q.step(1)
            self.assertTrue(q.irq(1))
            self.assertEqual(q.read32(BASEBAND + 0x1C), 1 << (5 + index))
            q.write32(BASEBAND + 0x20, 1 << (5 + index))
            self.assertFalse(q.irq(1))
        q.step(100_000_000)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)

    def test_timestamp_target1_deadline_survives_irq_mask_and_local_reset(self):
        q = self.machine()
        q.write32(BASEBAND + 0xE8, 2)
        q.write32(BASEBAND + 0xEC, 124)
        q.step(875_000)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        q.write32(BASEBAND + 0x400, 0x80000000)
        q.write32(BASEBAND + 0x18, 0x20)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0x20)
        q.write32(BASEBAND + 0x20, 0x20)
        q.write32(BASEBAND + 0xE8, 10)
        q.write32(BASEBAND + 0xEC, 624)
        q.write32(BASEBAND, 0x80000000)
        q.step(10_000_000)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)


    def test_pca_restore_target_does_not_request_a_phase_shift(self):
        q = self.machine()
        q.write32(BASEBAND + 0x18, 0x80FF)
        q.write32(BASEBAND + 0x544, 0x280000)
        q.write32(BASEBAND + 0x540, 0x10)
        q.write32(BASEBAND + 0x540, q.read32(BASEBAND + 0x544))
        self.assertEqual(q.read32(BASEBAND + 0x540), 0x280000)
        for invalid in (0x280001, 0x280008, 0x08000000):
            q.write32(BASEBAND + 0x540, invalid)
            self.assertEqual(q.read32(BASEBAND + 0x540), 0x280000)
        q.command("clock_step 1000000")
        q.write32(BASEBAND + 0x100, 1 << 31)
        self.assertEqual(q.read32(BASEBAND + 0x100), 3)
        self.assertEqual(q.read32(BASEBAND + 0x104), 499)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)

    def test_cp_digital_and_ble_configuration_preserves_other_fields(self):
        q = self.machine()

        def rmw(offset, clear=0, set_bits=0):
            value = (q.read32(BASEBAND + offset) & ~clear) | set_bits
            q.write32(BASEBAND + offset, value)
            self.assertEqual(q.read32(BASEBAND + offset), value)

        # Original CP digital initialization and modem/fast-ack configuration.
        rmw(0xC04, 0x00600000)
        rmw(0xD90, set_bits=0x00080000)
        rmw(0xD90, 0x0007C000, 0x00010000)
        rmw(0xCBC, 0x38000000, 0x10000000)
        rmw(0xD54, 0x007F0000, 0x00010000)
        rmw(0xD54, 0x00007F00, 0x00003C00)
        rmw(0xC14, 0x7F, 6)
        rmw(0xC3C, 0xC0000000, 0x40000000)
        rmw(0xC2C, 0xFE000000, 0x52000000)
        rmw(0xC0C, 0xFF000000, 0x28000000)
        rmw(0xC0C, 0x00FF0000, 0x002F0000)
        rmw(0xC8C, 1, 1)
        rmw(0xC8C, 0x10000000)
        rmw(0xC90, 0x7F000000, 0x12000000)
        rmw(0xC80, set_bits=0x4000)
        self.assertEqual(q.read32(BASEBAND + 0xCBC), 0x1000000A)
        self.assertEqual(q.read32(BASEBAND + 0xD54), 0x00013C00)
        self.assertEqual(q.read32(BASEBAND + 0xC8C), 0x47FC0135)
        self.assertEqual(q.read32(BASEBAND + 0xC90), 0x121A0000)
        self.assertNotIn("baseband unsupported",
                         q.stderr_path.read_text(errors="replace"))

    def test_radio_spi_configuration_does_not_start_or_complete_a_transfer(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0x80FF)
        self.assertEqual(q.read32(BASEBAND + 0x70), 0)
        self.assertEqual(q.read32(BASEBAND + 0x74), 0)
        q.write32(BASEBAND + 0x70, 0x3FFF00B0)
        self.assertEqual(q.read32(BASEBAND + 0x70), 0x3FFF00B0)
        q.write32(BASEBAND + 0x70, 0x00540010)
        self.assertEqual(q.read32(BASEBAND + 0x70), 0x00540010)
        # SPICOMP is read-only, including in an ordinary configuration RMW.
        q.write32(BASEBAND + 0x70, 0x00540012)
        self.assertEqual(q.read32(BASEBAND + 0x70), 0x00540010)
        # Reject the whole unsupported command, including its changed pointer.
        for value in (0x00100011, 0x00100013, 0x80000010, 0x00540014):
            q.write32(BASEBAND + 0x70, value)
            self.assertEqual(q.read32(BASEBAND + 0x70), 0x00540010)
        # Original initialization toggles DPCORR and verifies both levels.
        q.write32(BASEBAND + 0x74, 0x84202020)
        value = q.read32(BASEBAND + 0x74) | 0x40000000
        q.write32(BASEBAND + 0x74, value)
        q.write32(BASEBAND + 0x74, value & ~0x2000)
        self.assertEqual(q.read32(BASEBAND + 0x74), 0xC4200020)
        q.write32(BASEBAND + 0x74,
                  q.read32(BASEBAND + 0x74) | 0x2000)
        self.assertEqual(q.read32(BASEBAND + 0x74), 0xC4202020)
        q.write32(BASEBAND + 0x74, 0xC4202420)  # Unsupported bit 10.
        self.assertEqual(q.read32(BASEBAND + 0x74), 0xC4202020)
        q.command("clock_step 100000000")
        self.assertEqual(q.read32(BASEBAND + 0x70), 0x00540010)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertEqual(q.read32(BASEBAND + 0x410), 0)
        self.assertFalse(q.irq(0))
        self.assertFalse(q.irq(1))
        q.write32(BASEBAND, 0x80000000)
        self.assertEqual(q.read32(BASEBAND + 0x70), 0x00540010)
        self.assertEqual(q.read32(BASEBAND + 0x74), 0xC4202020)
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertEqual(q.read32(BASEBAND + 0x70), 0)
        self.assertEqual(q.read32(BASEBAND + 0x74), 0)

    def test_radio_rate_guard_and_delay_configuration_is_passive(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0x80FF)
        # Verified writes and final RMW values from original radio init.
        values = {0x50: 0x0000BCB1, 0x400: 0, 0x478: 0x00070040,
                  0x47C: 0x39003900, 0x490: 0x12, 0x534: 0x16,
                  0x878: 0x10C00040, 0x87C: 0xE4000400, 0x880: 0x00500755,
                  0x884: 0x00500755, 0x888: 0x0050073C, 0x88C: 0x0732,
                  0x890: 0x0608, 0x894: 0x020A,
                  0x898: 0x1EA0140C, 0x89C: 0x2600000C}
        for offset, value in values.items():
            reset_value = 0xE440E400 if offset == 0x87C else 0
            self.assertEqual(q.read32(BASEBAND + offset), reset_value)
            q.write32(BASEBAND + offset, value)
        q.write32(BASEBAND + 0x400, 0x2A)
        self.assertEqual(q.read32(BASEBAND + 0x400), 0x2A)
        q.write32(BASEBAND + 0x400, q.read32(BASEBAND + 0x400) & ~0x3F)
        # Support the full established LR flush field, not just cold bit 28.
        q.write32(BASEBAND + 0x878,
                  (q.read32(BASEBAND + 0x878) & ~0x1F000000) | 0x1F000000)
        self.assertEqual(q.read32(BASEBAND + 0x878), 0x1FC00040)
        q.write32(BASEBAND + 0x878, values[0x878])
        q.write32(BASEBAND + 0x48C, 0x00AA01BB)
        for clear, set_bits in ((0xFF0000, 0x420000), (0x7F00, 0x0C00),
                                (0xFF, 0x3C)):
            q.write32(BASEBAND + 0x48C,
                      (q.read32(BASEBAND + 0x48C) & ~clear) | set_bits)
        values[0x48C] = 0x00420C3C
        q.command("clock_step 100000000")
        for offset, value in values.items():
            self.assertEqual(q.read32(BASEBAND + offset), value)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertEqual(q.read32(BASEBAND + 0x410), 0)
        self.assertFalse(q.irq(0))
        self.assertFalse(q.irq(1))
        q.write32(BASEBAND, 0x80000000)
        for offset, value in values.items():
            self.assertEqual(q.read32(BASEBAND + offset), value)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset in values:
            reset_value = 0xE440E400 if offset == 0x87C else 0
            self.assertEqual(q.read32(BASEBAND + offset), reset_value)

    def test_ble_direction_finding_timing_is_passive_and_preserves_deadline(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        exchange = 0x6D020000
        sentinel = bytes(range(64))
        q.write(exchange, sentinel)
        q.write32(0x68006000, 0x12345678)
        q.write32(BASEBAND + 0xCA4, 0x68006000)
        q.write32(BASEBAND + 0xCAC, 0xAAAAAAAA)
        q.write32(BASEBAND + 0xC04, 0x03000000)
        q.write32(BASEBAND + 0x18, 0x80FF)
        q.write32(BASEBAND + 0xF8, 1)
        q.write32(BASEBAND + 0xFC, 624)
        q.step(100_000)

        # Original FC84 write order and captured S5 command values.
        cold = {0x980: 0x08180019, 0x984: 0x08180019,
                0x988: 0x08180019, 0x98C: 0x08180019, 0x994: 0}
        for offset, value in cold.items():
            self.assertEqual(q.read32(BASEBAND + offset), 0)
            q.write32(BASEBAND + offset, value)
            self.assertEqual(q.read32(BASEBAND + offset), value)
        # Independent full fields must work beyond the observed values.
        values = {0x980: 0x08180019, 0x984: 0xFE8000FF,
                  0x988: 0x80FF0001, 0x98C: 0xFFFF00FF, 0x994: 0xFFFF}
        for offset, value in values.items():
            q.write32(BASEBAND + offset, value)
            self.assertEqual(q.read32(BASEBAND + offset), value)
        # Field RMW can change timing without affecting the other bytes.
        q.write32(BASEBAND + 0x980,
                  (q.read32(BASEBAND + 0x980) & ~0x00FF0000) | 0x00A50000)
        values[0x980] = 0x08A50019
        q.write32(BASEBAND + 0x994, q.read32(BASEBAND + 0x994) & ~0x8080)
        values[0x994] = 0x7F7F
        self.assertEqual(q.read32(BASEBAND + 0xF8), 1)
        self.assertEqual(q.read32(BASEBAND + 0xFC), 624)
        q.step(212_499)
        self.assertFalse(q.irq(0))
        self.assertFalse(q.irq(1))
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        q.step(1)
        self.assertTrue(q.irq(1), "DF configuration must not move target 3")
        self.assertFalse(q.irq(0))
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        q.write32(BASEBAND + 0x20, TARGET3)
        q.step(100_000_000)
        self.assertFalse(q.irq(0))
        self.assertFalse(q.irq(1))
        for offset in (0x1C, 0x410, 0x460, 0x810, 0x860):
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        for offset, value in values.items():
            self.assertEqual(q.read32(BASEBAND + offset), value)
        self.assertEqual(q.read(exchange, len(sentinel)), sentinel)
        self.assertEqual(q.read32(0x68006000), 0x12345678)
        self.assertNotIn("baseband unsupported",
                         q.stderr_path.read_text(errors="replace"))

    def test_ble_direction_finding_masks_reset_and_unmodeled_neighbors(self):
        q = self.machine()
        values = {0x980: 0xFFFF00FF, 0x984: 0x12340056,
                  0x988: 0x789A00BC, 0x98C: 0xFEDC00BA, 0x994: 0xFFFF}
        for offset, value in values.items():
            q.write32(BASEBAND + offset, value)
            self.assertEqual(q.read32(BASEBAND + offset), value)
            unsupported = (0x10000, 0x80000000) if offset == 0x994 else (0x100, 0x8000)
            for bit in unsupported:
                # Rejection must preserve the old value, including fields
                # that the invalid mixed write would otherwise change.
                q.write32(BASEBAND + offset, 1 | bit)
                self.assertEqual(q.read32(BASEBAND + offset), value)
            for command in (f"writeb {BASEBAND + offset:#x} 0",
                            f"writew {BASEBAND + offset:#x} 0",
                            f"writel {BASEBAND + offset + 1:#x} 0"):
                q.command(command)
                self.assertEqual(q.read32(BASEBAND + offset), value)
        for offset in (0x990, 0x998):
            q.write32(BASEBAND + offset, 1)
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        log = q.stderr_path.read_text(errors="replace")
        for offset in (*values, 0x990, 0x998):
            self.assertIn(f"unsupported write offset={offset:#x}", log)
        # Match the established conservative configuration reset policy.
        q.write32(0x400800C4, 0x80)
        q.write32(BASEBAND, 0x80000000)
        for offset, value in values.items():
            self.assertEqual(q.read32(BASEBAND + offset), value)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset in values:
            self.assertEqual(q.read32(BASEBAND + offset), 0)
            q.write32(BASEBAND + offset, 0)
            self.assertEqual(q.read32(BASEBAND + offset), 0)

    def test_disabled_capture_and_relay_reads_preserve_state_and_deadline(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        offsets = (0xC50, 0xD70, 0xD04)
        sentinel = bytes(range(64))
        q.write(0x6D020000, sentinel)
        q.write32(0x68006000, 0x12345678)
        config = {0xC04: 0x03000000, 0xCA4: 0x68006000,
                  0xCAC: 0xAAAAAAAA, 0xD54: 0x21341205,
                  0xE68: 0x323C328C}
        for offset, value in config.items():
            q.write32(BASEBAND + offset, value)
        q.write32(BASEBAND + 0x18, 0x80FF)
        q.write32(BASEBAND + 0xF8, 1)
        q.write32(BASEBAND + 0xFC, 624)
        q.step(100_000)
        for _ in range(3):
            for offset in offsets:
                self.assertEqual(q.read32(BASEBAND + offset), 0)
        q.step(212_499)
        self.assertFalse(q.irq(1))
        q.step(1)
        self.assertTrue(q.irq(1), "disabled-state reads must not move target 3")
        for offset in offsets:
            self.assertEqual(q.read32(BASEBAND + offset), 0)
            self.assertTrue(q.irq(1), "configuration reads must not acknowledge IRQ")
            self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        q.write32(BASEBAND + 0x20, TARGET3)
        q.step(100_000_000)
        for irq in (0, 1, 2, 3, 6, 29):
            self.assertFalse(q.irq(irq))
        for offset in (0x1C, 0x410, 0x460, 0x810, 0x860):
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        for offset, value in config.items():
            self.assertEqual(q.read32(BASEBAND + offset), value)
        self.assertEqual(q.read(0x6D020000, len(sentinel)), sentinel)
        self.assertEqual(q.read32(0x68006000), 0x12345678)
        self.assertNotIn("baseband unsupported",
                         q.stderr_path.read_text(errors="replace"))
        q.write32(0x400800C4, 0x80)
        q.write32(BASEBAND, 0x80000000)
        for offset in offsets:
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset in offsets:
            self.assertEqual(q.read32(BASEBAND + offset), 0)

    def test_disabled_capture_replays_zero_and_rejects_operations_and_neighbors(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        offsets = (0xC50, 0xD70)
        q.write32(BASEBAND + 0x64, 0x12345678)
        q.write32(BASEBAND + 0x18, TARGET3)
        q.write32(BASEBAND + 0xF8, 1)
        q.write32(BASEBAND + 0xFC, 624)
        q.step(312_500)
        self.assertTrue(q.irq(1))
        q.write32(BASEBAND + 0xC50, 0x80000001)
        q.write32(BASEBAND + 0xD70, 0x10)
        self.assertEqual(q.read32(BASEBAND + 0xC50), 0x80000001)
        self.assertEqual(q.read32(BASEBAND + 0xD70), 0x10)
        for offset, invalid in ((0xC50, 0x40000000), (0xD70, 1)):
            q.write32(BASEBAND + offset, invalid)
            self.assertEqual(q.read32(BASEBAND + offset),
                             0x80000001 if offset == 0xC50 else 0x10)
            self.assertTrue(q.irq(1))
        for offset in offsets:
            # Common MMIO access-size/alignment rules must remain in force.
            for command in (f"readb {BASEBAND + offset:#x}",
                            f"readw {BASEBAND + offset:#x}",
                            f"readl {BASEBAND + offset + 1:#x}",
                            f"writeb {BASEBAND + offset:#x} 0",
                            f"writew {BASEBAND + offset:#x} 0",
                            f"writel {BASEBAND + offset + 1:#x} 0"):
                q.command(command)
                self.assertTrue(q.irq(1))
        neighbors = (0xC54, 0xD6C, 0xD74)
        for offset in neighbors:
            self.assertEqual(q.read32(BASEBAND + offset), 0)
            q.write32(BASEBAND + offset, 0)
            self.assertTrue(q.irq(1))
        self.assertEqual(q.read32(BASEBAND + 0x64), 0x12345678)
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        log = q.stderr_path.read_text(errors="replace")
        for offset in offsets:
            self.assertNotIn(f"unsupported read offset={offset:#x} ", log)
            self.assertNotIn(f"unsupported write offset={offset:#x} value=0x0", log)
        self.assertIn("unsupported write offset=0xc50 value=0x40000000", log)
        self.assertIn("unsupported write offset=0xd70 value=0x1", log)
        for offset in neighbors:
            self.assertIn(f"unsupported read offset={offset:#x} ", log)
            self.assertIn(f"unsupported write offset={offset:#x} value=0x0", log)
        self.assertNotIn("unsupported write offset=0xd00 value=0x0", log)
        self.assertNotIn("unsupported read offset=0xd00 ", log)
        q.write32(BASEBAND + 0x20, TARGET3)
        q.step(100_000_000)
        for irq in (0, 1, 2, 3, 6, 29):
            self.assertFalse(q.irq(irq))

    def test_relay_and_rf_access_counter_register(self):
        q = self.machine()
        q.write32(BASEBAND + 0xD04, 0x4000285A)
        self.assertEqual(q.read32(BASEBAND + 0xD04), 0x4000285A)
        q.write32(BASEBAND + 0xD04, 0xC000285A)
        self.assertEqual(q.read32(BASEBAND + 0xD04), 0x4000285A)
        self.assertIn("unsupported write offset=0xd04 value=0xc000285a",
                      q.stderr_path.read_text(errors="replace"))

    def test_pcm_private_data_format_registers(self):
        q = self.machine()
        q.write32(BASEBAND + 0xC9C, 0x00ABCDEF)
        q.write32(BASEBAND + 0xCA0, 0x000000A5)
        self.assertEqual(q.read32(BASEBAND + 0xC9C), 0x00ABCDEF)
        self.assertEqual(q.read32(BASEBAND + 0xCA0), 0x000000A5)
        q.write32(BASEBAND + 0xC9C, 0x80ABCDEF)
        q.write32(BASEBAND + 0xCA0, 0x000001A5)
        self.assertEqual(q.read32(BASEBAND + 0xC9C), 0x00ABCDEF)
        self.assertEqual(q.read32(BASEBAND + 0xCA0), 0x000000A5)
        log = q.stderr_path.read_text(errors="replace")
        self.assertIn("unsupported write offset=0xc9c value=0x80abcdef", log)
        self.assertIn("unsupported write offset=0xca0 value=0x1a5", log)

    def test_tws_sync_clock_and_trigger_clear_commands(self):
        q = self.machine()
        # C84 is a retained 28-bit synchronization clock value.
        q.write32(BASEBAND + 0xC84, 0x01234567)
        self.assertEqual(q.read32(BASEBAND + 0xC84), 0x01234567)

        # Arm route 0, verify the live trigger flag, then issue the same
        # route-clear command used by the driver before programming a route.
        q.write32(BASEBAND + 0xC94, 1)
        q.write32(BASEBAND + 0xC98, 0x80000000)
        self.assertEqual(q.read32(BASEBAND + 0xE60) & 0x80, 0x80)
        q.write32(BASEBAND + 0xC08, 1 << 4)
        self.assertEqual(q.read32(BASEBAND + 0xE60) & 0x80, 0)
        q.step(1_000_000)
        self.assertFalse(q.irq(1))

        # Trigger-clear command bits are write-only; unknown command bits
        # remain fail-closed.
        self.assertEqual(q.read32(BASEBAND + 0xC08), 0)
        q.write32(BASEBAND + 0xC08, 1 << 26)
        self.assertEqual(q.read32(BASEBAND + 0xC08), 0)
        self.assertIn("unsupported write offset=0xc08 value=0x4000000",
                      q.stderr_path.read_text(errors="replace"))

        q.qmp("system_reset")
        self.assertEqual(q.read32(BASEBAND + 0xC84), 0)

    def test_sync_interrupt_latch_and_register_field_boundaries(self):
        q = self.machine()
        sync = BASEBAND + 0xD28
        cntl1 = BASEBAND + 0xC08

        # The trigger helper sets the level latch before changing routes.
        q.write32(sync, 0x80000000)
        self.assertEqual(q.read32(sync), 0x80000000)
        q.write32(cntl1, 1 << 14)
        self.assertEqual(q.read32(sync), 0)

        # Only bit 31 belongs to this register; rejection is atomic.
        q.write32(sync, 0x80000000)
        q.write32(sync, 0x80000001)
        self.assertEqual(q.read32(sync), 0x80000000)

        # The BLE MTK and FEC words now reject gaps/reserved upper bits while
        # retaining every field defined by the controller register layout.
        q.write32(BASEBAND + 0xD78, 0x57FF007F)
        self.assertEqual(q.read32(BASEBAND + 0xD78), 0x57FF007F)
        q.write32(BASEBAND + 0xD78, 0x57FF807F)
        self.assertEqual(q.read32(BASEBAND + 0xD78), 0x57FF007F)
        q.write32(BASEBAND + 0xD90, 0x1FFFFFFF)
        self.assertEqual(q.read32(BASEBAND + 0xD90), 0x1FFFFFFF)
        q.write32(BASEBAND + 0xD90, 0xFFFFFFFF)
        self.assertEqual(q.read32(BASEBAND + 0xD90), 0x1FFFFFFF)

        q.qmp("system_reset")
        self.assertEqual(q.read32(sync), 0)
        log = q.stderr_path.read_text(errors="replace")
        self.assertIn("unsupported write offset=0xd28 value=0x80000001", log)
        self.assertIn("unsupported write offset=0xd78 value=0x57ff807f", log)
        self.assertIn("unsupported write offset=0xd90 value=0xffffffff", log)

    def test_radio_unverified_fields_reject_without_changing_configuration(self):
        q = self.machine()
        for offset, value, unsupported in (
                (0x50, 0x0000BCB1, 0x40),
                (0x400, 0x2A, 0x40000000),
                (0x478, 0x00070040, 0x4000),
                (0x47C, 0x39003900, 0x4000),
                (0x48C, 0x00420C3C, 0x8000),
                (0x490, 0x12, 0x80),
                (0x534, 0x16, 0x40),
                (0x878, 0x10C00040, 0x4000),
                (0x87C, 0xE4000400, 0x00800000),
                (0x880, 0x00500755, 0x8000),
                (0x884, 0x00500755, 0x8000),
                (0x888, 0x0050073C, 0x8000),
                (0x88C, 0x0732, 0x10000),
                (0x890, 0x0608, 0x8000),
                (0x894, 0x020A, 0x8000),
                (0x898, 0x1EA0140C, 0x80),
                (0x89C, 0x2600000C, 0x100)):
            q.write32(BASEBAND + offset, value)
            q.write32(BASEBAND + offset, value | unsupported)
            self.assertEqual(q.read32(BASEBAND + offset), value)
        # Unknown trigger-register fields remain unsupported.
        previous = q.read32(BASEBAND + 0xC80)
        q.write32(BASEBAND + 0xC80, previous | 0x1000)
        self.assertEqual(q.read32(BASEBAND + 0xC80), previous)
        log = q.stderr_path.read_text(errors="replace")
        self.assertIn("unsupported write offset=0x534", log)
        self.assertIn("unsupported write offset=0xc80", log)

    def test_original_zero_context_restore_preserves_state_and_deadline(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        sentinel = bytes(range(64))
        q.write(0x6D020000, sentinel)
        q.write32(0x68006000, 0x12345678)
        config = {0xC04: 0x03000000, 0xCA4: 0x68006000,
                  0xCAC: 0xAAAAAAAA, 0xD54: 0x21341205}
        for offset, value in config.items():
            q.write32(BASEBAND + offset, value)
        q.write32(BASEBAND + 0x18, 0x80FF)
        q.write32(BASEBAND + 0xF8, 1)
        q.write32(BASEBAND + 0xFC, 624)
        q.step(100_250)
        saved = {offset: q.read32(BASEBAND + offset)
                 for offset in ZERO_RESTORE_OFFSETS}
        for _ in range(2):
            for offset, value in saved.items():
                self.assertEqual(value, 0)
                q.write32(BASEBAND + offset, value)
                self.assertEqual(q.read32(BASEBAND + offset), 0)
        q.write32(BASEBAND + 0x100, 0x80000000)
        self.assertEqual(q.read32(BASEBAND + 0x100), 0)
        self.assertEqual(q.read32(BASEBAND + 0x104), 424)
        self.assertEqual(q.read32(BASEBAND + 0x9C4), 100)
        q.step(212_249)
        self.assertFalse(q.irq(1))
        q.step(1)
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        for offset, value in saved.items():
            q.write32(BASEBAND + offset, value)
            self.assertEqual(q.read32(BASEBAND + offset), 0)
            self.assertTrue(q.irq(1))
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        q.write32(BASEBAND + 0x20, TARGET3)
        q.step(100_000_000)
        for irq in (0, 1, 2, 3, 6, 29):
            self.assertFalse(q.irq(irq))
        for offset in (0x1C, 0x410, 0x460, 0x810, 0x860):
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        for offset, value in config.items():
            self.assertEqual(q.read32(BASEBAND + offset), value)
        self.assertEqual(q.read(0x6D020000, len(sentinel)), sentinel)
        self.assertEqual(q.read32(0x68006000), 0x12345678)
        q.write32(0x400800C4, 0x80)
        q.write32(BASEBAND, 0x80000000)
        for offset, value in saved.items():
            q.write32(BASEBAND + offset, value)
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset, value in saved.items():
            q.write32(BASEBAND + offset, value)
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        self.assertNotIn("baseband unsupported",
                         q.stderr_path.read_text(errors="replace"))

    def test_zero_context_restore_rejects_nonzero_commands_and_neighbors(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0x80FF)
        q.write32(BASEBAND + 0xF8, 1)
        q.write32(BASEBAND + 0xFC, 624)
        q.step(312_500)
        # Include frequency START, FA clear/start, high fields and all bits.
        invalid_values = (1, 2, 0x80, 0x800, 0x80000000, 0xFFFFFFFF)
        for offset in ZERO_RESTORE_OFFSETS:
            q.write32(BASEBAND + offset, 0)
            for value in invalid_values:
                q.write32(BASEBAND + offset, value)
                self.assertEqual(q.read32(BASEBAND + offset), 0)
                self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
                self.assertTrue(q.irq(1))
            for command in (f"writeb {BASEBAND + offset:#x} 0",
                            f"writew {BASEBAND + offset:#x} 0",
                            f"writel {BASEBAND + offset + 1:#x} 0"):
                q.command(command)
                self.assertEqual(q.read32(BASEBAND + offset), 0)
                self.assertTrue(q.irq(1))
        neighbors = (0x190, 0x198, 0x470, 0x608, 0x830, 0x938, 0x998,
                     0x9B8, 0x9EC, 0xA10, 0xCE0, 0xD60)
        for offset in neighbors:
            q.write32(BASEBAND + offset, 0)
        q.write32(BASEBAND + 0xD00, 1)
        q.write32(BASEBAND + 0x540, 0x10)
        q.write32(BASEBAND + 0x540, 0x00280001)
        self.assertEqual(q.read32(BASEBAND + 0x540), 0x10)
        log = q.stderr_path.read_text(errors="replace")
        for offset in ZERO_RESTORE_OFFSETS:
            self.assertNotIn(f"unsupported read offset={offset:#x} ", log)
            self.assertNotIn(f"unsupported write offset={offset:#x} value=0x0", log)
            for value in invalid_values:
                self.assertIn(f"unsupported write offset={offset:#x} value={value:#x}",
                              log)
        for offset in neighbors:
            self.assertIn(f"unsupported write offset={offset:#x} value=0x0", log)
        self.assertIn("unsupported write offset=0x540 value=0x280001", log)
        for offset, value, invalid in ((0xC, 0x10000, 1), (0x9D0, 0xFF, 0x100)):
            q.write32(BASEBAND + offset, value)
            q.write32(BASEBAND + offset, value | invalid)
            self.assertEqual(q.read32(BASEBAND + offset), value)
            q.write32(BASEBAND + offset, 0)
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        # The accepted +d00 context-restore pulse clears the pending
        # transient status before the subsequent protected-field checks.
        self.assertFalse(q.irq(1))
        q.write32(BASEBAND + 0x20, TARGET3)
        q.step(100_000_000)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))

    def test_mac_context_restore_pulse_clears_transient_state_and_preserves_config(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0xC74, 0x12345678)
        q.write32(BASEBAND + 0x18, TARGET3)
        q.write32(BASEBAND + 0xF8, 1)
        q.write32(BASEBAND + 0xFC, 624)
        q.step(312_500)
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        self.assertTrue(q.irq(1))
        q.write32(BASEBAND + 0xD00, 1)
        self.assertEqual(q.read32(BASEBAND + 0xD00), 0)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))
        self.assertEqual(q.read32(BASEBAND + 0xC74), 0x12345678)
        q.write32(BASEBAND + 0xD00, 0)
        q.write32(BASEBAND + 0xD00, 2)
        self.assertEqual(q.read32(BASEBAND + 0xD00), 0)
        self.assertIn("unsupported write offset=0xd00 value=0x2",
                      q.stderr_path.read_text(errors="replace"))

    def test_retained_vendor_fields_and_fa_mode_do_not_create_operations(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0x80FF)
        self.assertEqual(q.read32(BASEBAND + 0xCCC), 0)
        q.write32(BASEBAND + 0xCCC, 0x00FF7FFF)
        self.assertEqual(q.read32(BASEBAND + 0xCCC), 0x00FF7FFF)
        # The two original RMWs preserve the other established field.
        q.write32(BASEBAND + 0xCCC,
                  (q.read32(BASEBAND + 0xCCC) & ~0xFF0000) | 0x140000)
        self.assertEqual(q.read32(BASEBAND + 0xCCC), 0x00147FFF)
        q.write32(BASEBAND + 0xCCC,
                  (q.read32(BASEBAND + 0xCCC) & ~0x7FFF) | 0x2F)
        self.assertEqual(q.read32(BASEBAND + 0xCCC), 0x0014002F)
        for invalid in (0x00008000, 0x01000000, 0x80000000):
            q.write32(BASEBAND + 0xCCC, 0x00150030 | invalid)
            self.assertEqual(q.read32(BASEBAND + 0xCCC), 0x0014002F)
        original_mode = q.read32(BASEBAND + 0xC80)
        q.write32(BASEBAND + 0xC80, original_mode | 2)
        self.assertEqual(q.read32(BASEBAND + 0xC80), original_mode | 2)
        q.command("clock_step 100000000")
        self.assertEqual(q.read32(BASEBAND + 0xCCC), 0x0014002F)
        self.assertEqual(q.read32(BASEBAND + 0xC80), original_mode | 2)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertEqual(q.read32(BASEBAND + 0x410), 0)
        self.assertFalse(q.irq(0))
        self.assertFalse(q.irq(1))
        q.write32(BASEBAND, 0x80000000)
        self.assertEqual(q.read32(BASEBAND + 0xCCC), 0x0014002F)
        self.assertEqual(q.read32(BASEBAND + 0xC80), original_mode | 2)
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertEqual(q.read32(BASEBAND + 0xCCC), 0)
        self.assertEqual(q.read32(BASEBAND + 0xC80), 1)

    def test_vendor_retained_word_and_high_modes_reject_trigger_operations(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0x80FF)
        for offset in (0xC38, 0xC74, 0xC7C, 0xCC0, 0xE74):
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        self.assertEqual(q.read32(BASEBAND + 0xC88), 0x00003B8F)
        # C74 is explicitly an opaque retained word, without invented fields.
        q.write32(BASEBAND + 0xC74, 0x013E0140)
        saved_word = q.read32(BASEBAND + 0xC74)
        q.write32(BASEBAND + 0xC74, 0xA5C31234)
        self.assertEqual(q.read32(BASEBAND + 0xC74), 0xA5C31234)
        q.write32(BASEBAND + 0xC74, saved_word)
        # CC0 is another retained whole word; its internal fields are unknown.
        q.write32(BASEBAND + 0xCC0, 0x000F1723)
        saved_cc0 = q.read32(BASEBAND + 0xCC0)
        q.write32(BASEBAND + 0xCC0, 0xF13579BD)
        self.assertEqual(q.read32(BASEBAND + 0xCC0), 0xF13579BD)
        q.write32(BASEBAND + 0xCC0, saved_cc0)
        q.write32(BASEBAND + 0xC38, 0x12345678)
        q.write32(BASEBAND + 0xC38, q.read32(BASEBAND + 0xC38) | 0x40000000)
        self.assertEqual(q.read32(BASEBAND + 0xC38), 0x52345678)
        q.write32(BASEBAND + 0xC38, q.read32(BASEBAND + 0xC38) & ~0x40000000)
        # Bit 31 is the BT/WiFi coexistence selector and is a real
        # controller field, so it must survive an ordinary RMW sequence.
        q.write32(BASEBAND + 0xC38, 0x80000001)
        self.assertEqual(q.read32(BASEBAND + 0xC38), 0x80000001)
        q.write32(BASEBAND + 0xC38, 0x12345678)
        for value in (0x10000, 0x20000, 0x100000, 0, 0x130000):
            q.write32(BASEBAND + 0xE74, value)
            self.assertEqual(q.read32(BASEBAND + 0xE74), value)
        # Original BIC of unknown bit 23 preserves the modeled relay fields.
        q.write32(BASEBAND + 0xE74, q.read32(BASEBAND + 0xE74) & ~0x800000)
        self.assertEqual(q.read32(BASEBAND + 0xE74), 0x130000)
        # Setting that bit is still unsupported, including changed valid data.
        q.write32(BASEBAND + 0xE74, 0x810000)
        self.assertEqual(q.read32(BASEBAND + 0xE74), 0x130000)
        # Original cold, later initialization and RF-mode clear sequences.
        q.write32(BASEBAND + 0xC7C, q.read32(BASEBAND + 0xC7C) | 0x00300000)
        q.write32(BASEBAND + 0xC7C, q.read32(BASEBAND + 0xC7C) | 0x01000000)
        self.assertEqual(q.read32(BASEBAND + 0xC7C), 0x01300000)
        q.write32(BASEBAND + 0xC7C,
                  (q.read32(BASEBAND + 0xC7C) & ~0x3E000000) | 0x02000000)
        self.assertEqual(q.read32(BASEBAND + 0xC7C), 0x03300000)
        q.write32(BASEBAND + 0xC7C, q.read32(BASEBAND + 0xC7C) & ~0x01000000)
        self.assertEqual(q.read32(BASEBAND + 0xC7C), 0x02300000)
        for unsupported in (2, 0x20, 0x80, 0x00800000,
                            0x40000000, 0x80000000):
            q.write32(BASEBAND + 0xC7C, 0x01200000 | unsupported)
            self.assertEqual(q.read32(BASEBAND + 0xC7C), 0x02300000)
        # C88 is a complete PCM control register. Its documented fields
        # include the frame count, write enable, fixed-data controls and
        # enhanced PCM mode, so low bits are valid writes rather than
        # unsupported accesses.
        q.write32(BASEBAND + 0xC88, 0xF3A5C17F)
        self.assertEqual(q.read32(BASEBAND + 0xC88), 0xF3A5C17F)
        q.write32(BASEBAND + 0xC88,
                  (q.read32(BASEBAND + 0xC88) & ~0x00007F00) | 0x00002A00)
        self.assertEqual(q.read32(BASEBAND + 0xC88), 0xF3A5AA7F)
        q.command("clock_step 100000000")
        expected = {0xC38: 0x12345678, 0xC74: saved_word,
                    0xC7C: 0x02300000, 0xC88: 0xF3A5AA7F, 0xCC0: saved_cc0,
                    0xE74: 0x130000}
        for offset, value in expected.items():
            self.assertEqual(q.read32(BASEBAND + offset), value)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertEqual(q.read32(BASEBAND + 0x410), 0)
        self.assertFalse(q.irq(0))
        self.assertFalse(q.irq(1))
        q.write32(BASEBAND, 0x80000000)
        for offset, value in expected.items():
            self.assertEqual(q.read32(BASEBAND + offset), value)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset in expected:
            reset_value = 0x3B8F if offset == 0xC88 else 0
            self.assertEqual(q.read32(BASEBAND + offset), reset_value)

    def test_fast_ack_software_registers_are_retained(self):
        q = self.machine()
        for offset, first, second in (
            (0xCC4, 0x13579BDF, 0xA5C31234),
            (0xCC8, 0x2468ACE0, 0x5A3C21D4),
        ):
            q.write32(BASEBAND + offset, first)
            self.assertEqual(q.read32(BASEBAND + offset), first)
            q.write32(BASEBAND + offset, second)
            self.assertEqual(q.read32(BASEBAND + offset), second)
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertEqual(q.read32(BASEBAND + 0xCC4), 0)
        self.assertEqual(q.read32(BASEBAND + 0xCC8), 0)

    def test_event_configuration_never_writes_the_configured_target(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(0x68006000, 0x12345678)
        q.write32(0x68006004, 0x87654321)
        q.write32(BASEBAND + 0xCA4, 0x68006000)
        q.write32(BASEBAND + 0xCA8, 0x68006004)
        q.write32(BASEBAND + 0xCAC, 0xAAAAAAAA)
        q.write32(BASEBAND + 0xCB4, 0xBBBBBBBB)
        q.write32(BASEBAND + 0xC04, 0x03000000)
        q.write32(BASEBAND + 0x18, 0x800E)
        q.write32(BASEBAND + 0xD78, 0x50000000)
        q.command("clock_step 100000000")
        self.assertEqual(q.read32(0x68006000), 0x12345678)
        self.assertEqual(q.read32(0x68006004), 0x87654321)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertEqual(q.read32(BASEBAND + 0x410), 0)
        self.assertFalse(q.irq(0))
        self.assertFalse(q.irq(1))

    def test_bt_ble_cold_core_enable_does_not_produce_radio_activity(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0x80FF)
        q.write32(0x68006000, 0x12345678)
        q.write32(BASEBAND + 0xCA4, 0x68006000)
        q.write32(BASEBAND + 0xCAC, 0xAAAAAAAA)
        q.write32(BASEBAND + 0xC04, 0x03000000)
        # Original BT cold path configures the RX ring, masks, then enable.
        q.write32(BASEBAND + 0x450, 0)
        q.write32(BASEBAND + 0x428, 0x17C8)
        q.write32(BASEBAND + 0x400, 0x0D)
        q.write32(BASEBAND + 0x40C, 0x10016)
        q.write32(BASEBAND + 0x414, 0xFFFFFFFF)
        q.write32(BASEBAND + 0x400, q.read32(BASEBAND + 0x400) | 0x100)
        # Verified BLE cold control subset, including the optional mask bit.
        # List/seed initialization is exercised separately below.
        q.write32(BASEBAND + 0x850, 0x80808080)
        q.write32(BASEBAND + 0x800, 0x00100607)
        q.write32(BASEBAND + 0x80C, 0x1007E)
        q.write32(BASEBAND + 0x80C, q.read32(BASEBAND + 0x80C) | 0x100)
        q.write32(BASEBAND + 0x828, 0x2D0)
        q.write32(BASEBAND + 0x800, q.read32(BASEBAND + 0x800) | 0x100)
        expected = {0x400: 0x10D, 0x40C: 0x10016, 0x428: 0x17C8,
                    0x450: 0, 0x800: 0x00100707, 0x80C: 0x1017E,
                    0x828: 0x2D0, 0x850: 0x80808080}
        for enabled in (True, False, True):
            for offset in (0x400, 0x800):
                expected[offset] = (expected[offset] & ~0x100) | (
                    0x100 if enabled else 0)
                q.write32(BASEBAND + offset, expected[offset])
            q.command("clock_step 100000000")
            for offset, value in expected.items():
                self.assertEqual(q.read32(BASEBAND + offset), value)
            for offset in (0x1C, 0x410, 0x460, 0x810, 0x860):
                self.assertEqual(q.read32(BASEBAND + offset), 0)
            self.assertEqual(q.read32(0x68006000), 0x12345678)
            self.assertFalse(q.irq(0))
            self.assertFalse(q.irq(1))
            self.assertFalse(q.irq(2))
        self.assertNotIn("baseband unsupported",
                         q.stderr_path.read_text(errors="replace"))

    def test_core_configuration_rejects_local_commands_and_unknown_fields(self):
        q = self.machine()
        cases = (
            (0x400, 0x10D, (0x80000001, 0x40000001, 0x08000001, 0x41)),
            (0x40C, 0x10016, (0x9, 0x101, 0x200001)),
            (0x428, 0x17C8, (0x4001, 0x80000001)),
            (0x800, 0x00100707, (0x80000001, 0x40000001, 0x10000001,
                                  0x08000001, 0x04000001, 0x02000001,
                                  0x01000001, 0x801, 0x11)),
            (0x80C, 0x1017E, (0x201, 0x20001)),
            (0x828, 0x2D0, (0x4001, 0x80000001)),
        )
        for offset, original, invalid_values in cases:
            q.write32(BASEBAND + offset, original)
            for invalid in invalid_values:
                # The supported part changes too: reject the whole write.
                q.write32(BASEBAND + offset, invalid)
                self.assertEqual(q.read32(BASEBAND + offset), original)
        log = q.stderr_path.read_text(errors="replace")
        for offset in (0x400, 0x40C, 0x428, 0x800, 0x80C, 0x828):
            self.assertIn(f"unsupported write offset=0x{offset:x}", log)

    def test_core_configuration_full_fields_and_machine_reset(self):
        q = self.machine()
        values = {0x400: 0x13F, 0x40C: 0x10017, 0x428: 0x3FFF,
                  0x450: 0x7F80FF41, 0x800: 0x0010070F, 0x80C: 0x101FF,
                  0x828: 0x3FFF, 0x850: 0xFF417F80}
        for offset, value in values.items():
            q.write32(BASEBAND + offset, value)
            self.assertEqual(q.read32(BASEBAND + offset), value)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset in values:
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        for offset in (0x410, 0x460, 0x810, 0x860):
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        self.assertNotIn("baseband unsupported",
                         q.stderr_path.read_text(errors="replace"))

    def test_ble_cold_list_and_seed_setup_does_not_search_or_generate_events(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 0x80FF)
        q.write32(BASEBAND + 0x80C, 0x101FF)
        q.write32(BASEBAND + 0x800, 0x00100707)
        # Real original WPAL/RAL bases. Configuration must not touch entries.
        table_words = {0x6D021118: 0x11223344, 0x6D02111C: 0x55667788,
                       0x6D021220: 0xAABBCCDD, 0x6D021224: 0x12345678}
        for address, value in table_words.items():
            q.write32(address, value)
        config = {0x930: 0xFF9602D9, 0x940: 0x00160446, 0x944: 0x446,
                  0x970: 0x00140244, 0x974: 0x244, 0x948: 0x10, 0x9E0: 1}
        for offset, value in config.items():
            q.write32(BASEBAND + offset, value)
        q.write32(BASEBAND + 0x978, 0x80123456)
        q.write32(BASEBAND + 0x97C, 0x80234567)
        for _ in range(3):
            q.command("clock_step 100000000")
            for offset, value in config.items():
                self.assertEqual(q.read32(BASEBAND + offset), value)
            self.assertEqual(q.read32(BASEBAND + 0x978), 0x123456)
            self.assertEqual(q.read32(BASEBAND + 0x97C), 0x234567)
            for address, value in table_words.items():
                self.assertEqual(q.read32(address), value)
            for offset in (0x1C, 0x410, 0x460, 0x810, 0x860):
                self.assertEqual(q.read32(BASEBAND + offset), 0)
            for irq in (0, 1, 2):
                self.assertFalse(q.irq(irq))
        self.assertNotIn("baseband unsupported",
                         q.stderr_path.read_text(errors="replace"))

    def test_ble_list_configuration_bounds_and_whole_machine_reset(self):
        q = self.machine()
        cases = ((0x930, 0xFFFF3FFF, (0x4001, 0x8001)),
                 (0x940, 0x00FF3FFF, (0x4001, 0x01000001)),
                 (0x944, 0x3FFF, (0x4001, 0x8001, 0x10001)),
                 (0x948, 0x3F, (0x41, 0x80000001)),
                 (0x970, 0x00FF3FFF, (0x4001, 0x8001, 0x01000001)),
                 (0x974, 0x3FFF, (0x4001, 0x8001, 0x10001)),
                 (0x9E0, 0xFF, (0x102, 0x80000002)))
        for offset, value, invalid_values in cases:
            q.write32(BASEBAND + offset, value)
            self.assertEqual(q.read32(BASEBAND + offset), value)
            for invalid in invalid_values:
                q.write32(BASEBAND + offset, invalid)
                self.assertEqual(q.read32(BASEBAND + offset), value)
        # The existing idle global reset preserves configuration by policy.
        q.write32(BASEBAND, 0x80000000)
        for offset, value, _ in cases:
            self.assertEqual(q.read32(BASEBAND + offset), value)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset, _, _ in cases:
            self.assertEqual(q.read32(BASEBAND + offset), 0)

    def test_ble_seed_load_requires_init_and_preserves_independent_state(self):
        q = self.machine()
        q.write32(BASEBAND + 0x978, 0x803FFFFF)
        q.write32(BASEBAND + 0x97C, 0x80123456)
        self.assertEqual(q.read32(BASEBAND + 0x978), 0x3FFFFF)
        self.assertEqual(q.read32(BASEBAND + 0x97C), 0x123456)
        for offset, previous in ((0x978, 0x3FFFFF), (0x97C, 0x123456)):
            for invalid in (0, 1, 0x3FFFFF, 0x80400001, 0xC0000001,
                            0xFFFFFFFF):
                q.write32(BASEBAND + offset, invalid)
                self.assertEqual(q.read32(BASEBAND + offset), previous)
        # Explicitly initializing to zero is valid, unlike a write of zero.
        q.write32(BASEBAND + 0x978, 0x80000000)
        self.assertEqual(q.read32(BASEBAND + 0x978), 0)
        self.assertEqual(q.read32(BASEBAND + 0x97C), 0x123456)
        q.write32(BASEBAND + 0x978, 0x80234567)
        q.write32(BASEBAND, 0x80000000)
        self.assertEqual(q.read32(BASEBAND + 0x978), 0x234567)
        self.assertEqual(q.read32(BASEBAND + 0x97C), 0x123456)
        log = q.stderr_path.read_text(errors="replace")
        self.assertIn("unsupported write offset=0x978", log)
        self.assertIn("unsupported write offset=0x97c", log)
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertEqual(q.read32(BASEBAND + 0x978), 0)
        self.assertEqual(q.read32(BASEBAND + 0x97C), 0)

    def test_status_is_read_only_and_acknowledgments_create_no_pending_bits(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        for offset in (0x1C, 0x410, 0x460, 0x810, 0x860):
            q.write32(BASEBAND + offset, 0xFFFFFFFF)
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        for offset in (0x20, 0x414, 0x814):
            q.write32(BASEBAND + offset, 0xFFFFFFFF)
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        q.write32(BASEBAND + 0x18, 0x80FF)
        self.assertEqual(q.read32(BASEBAND + 0x18), 0x80FF)
        q.write32(BASEBAND + 0x18, 0x800E)
        self.assertEqual(q.read32(BASEBAND + 0x18), 0x800E)
        self.assertFalse(q.irq(0))
        self.assertFalse(q.irq(1))

    def test_reset_command_completes_without_modifying_extension_or_mailbox(self):
        q = self.machine()
        q.write32(BASEBAND + 0x64, 0x01234567)
        q.write32(BASEBAND + 0xC04, 0x03000000)
        q.write32(BASEBAND + 0x18, 0x800E)
        q.write32(0x68006000, 0x76543210)
        q.write32(BASEBAND, 0x80000000)
        self.assertEqual(q.read32(BASEBAND), 0)
        self.assertEqual(q.read32(BASEBAND + 0x64), 0x01234567)
        self.assertEqual(q.read32(BASEBAND + 0xC04), 0x03000000)
        self.assertEqual(q.read32(BASEBAND + 0x18), 0x800E)
        self.assertEqual(q.read32(0x68006000), 0x76543210)
        # A CPU reset has no established ownership of baseband configuration.
        q.write32(0x400800C8, 0x80)
        self.assertEqual(q.read32(BASEBAND + 0xC04), 0x03000000)

    def test_unknown_offsets_and_fields_fail_without_storage_fallback(self):
        q = self.machine()
        q.write32(BASEBAND + 0xC04, 0x03000000)
        q.write32(BASEBAND + 0xC04, 0x00000040)
        self.assertEqual(q.read32(BASEBAND + 0xC04), 0x03000000)
        q.write32(BASEBAND + 0x18, 0x800E)
        q.write32(BASEBAND + 0x18, 0x10000)
        self.assertEqual(q.read32(BASEBAND + 0x18), 0x800E)
        # Radio-control reset remains unmodeled, unlike qualified SWINT.
        q.write32(BASEBAND, 0x10000000)
        for offset in (0x38, 0x40, 0x44, 0x4C, 0x108, 0xFFC):
            q.write32(BASEBAND + offset, 0xFFFFFFFF)
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        log = q.stderr_path.read_text(errors="replace")
        self.assertIn("BTC baseband unsupported write offset=0xc04", log)
        self.assertIn("BTC baseband unsupported write offset=0x0 ", log)
        self.assertIn("BTC baseband unsupported read offset=0x108", log)

    def test_clock_sample_is_atomic_and_reads_do_not_advance_time(self):
        q = self.machine()

        def sample():
            q.write32(BASEBAND + 0x100,
                      q.read32(BASEBAND + 0x100) | 0x80000000)
            return tuple(q.read32(BASEBAND + off)
                         for off in (0x100, 0x104, 0x9C4))

        self.assertEqual(sample(), (0, 624, 0))
        q.command("clock_step 500")
        self.assertEqual(sample(), (0, 623, 0))
        q.command("clock_step 312000")
        self.assertEqual(sample(), (1, 624, 312))
        q.command("clock_step 500")
        self.assertEqual(sample(), (1, 623, 313))
        q.command("clock_step 1000000")
        # Every read still belongs to the previous sampling request.
        self.assertEqual(q.read32(BASEBAND + 0x100), 1)
        q.command("clock_step 500")
        self.assertEqual(q.read32(BASEBAND + 0x104), 623)
        q.command("clock_step 500")
        self.assertEqual(q.read32(BASEBAND + 0x9C4), 313)
        self.assertEqual(sample(), (4, 496, 1314))
        q.write32(BASEBAND + 0x100, 0x0FFFFFFF)
        q.write32(BASEBAND + 0x104, 0)
        q.write32(BASEBAND + 0x9C4, 0)
        self.assertEqual(tuple(q.read32(BASEBAND + off)
                               for off in (0x100, 0x104, 0x9C4)),
                         (4, 496, 1314))
        q.write32(BASEBAND + 0x100, 0xC0000000 | 7)
        self.assertEqual(q.read32(BASEBAND + 0x100), 7)
        self.assertEqual(q.read32(BASEBAND + 0x104), 496)
        self.assertNotIn("unsupported write offset=0x100",
                         q.stderr_path.read_text(errors="replace"))
        q.write32(BASEBAND, 0x80000000)
        self.assertEqual(sample(), (7, 496, 1314))

    def test_clock_update_and_iso_update_commands_rebase_live_counters(self):
        q = self.machine()

        # CLKN_UPD replaces the slot count while preserving the fine phase.
        q.command("clock_step 123456")
        q.write32(BASEBAND + 0x100, 0xC0000000 | 5)
        self.assertEqual(q.read32(BASEBAND + 0x100), 5)
        self.assertEqual(q.read32(BASEBAND + 0x104), 624 - ((123456 // 500) % 625))
        self.assertEqual(q.read32(BASEBAND + 0x9C4), 123)

        # ISO_UPD applies the programmed microsecond and half-us correction;
        # ISO_SAMP then exposes that new count through the read-only latch.
        q.write32(BASEBAND + 0x9C8, 10)
        q.write32(BASEBAND + 0x9CC, 1)
        q.write32(BASEBAND + 0x9C0, 0xC0000001)
        self.assertEqual(q.read32(BASEBAND + 0x9C0), 1)
        self.assertEqual(q.read32(BASEBAND + 0x9C4), 133)
        self.assertNotIn("unsupported write offset=0x100",
                         q.stderr_path.read_text(errors="replace"))
        self.assertNotIn("unsupported write offset=0x9c0",
                         q.stderr_path.read_text(errors="replace"))

    def test_clock_counter_wrap_and_machine_reset(self):
        q = self.machine()
        # The ISO timestamp wraps at 2^32 us; SCLK independently at 2^28 hs.
        elapsed = (1 << 32) * 1000 + 500
        q.command(f"clock_step {elapsed}")
        q.write32(BASEBAND + 0x100, 0x80000000)
        self.assertEqual(q.read32(BASEBAND + 0x9C4), 0)
        self.assertEqual(q.read32(BASEBAND + 0x100),
                         (elapsed // 500 // 625) & 0x0FFFFFFF)
        slot_wrap = (1 << 28) * 312500
        q.command(f"clock_step {slot_wrap - elapsed}")
        q.write32(BASEBAND + 0x100, 0x80000000)
        self.assertEqual(q.read32(BASEBAND + 0x100), 0)
        self.assertEqual(q.read32(BASEBAND + 0x104), 624)
        self.assertEqual(q.read32(BASEBAND + 0x9C4),
                         (slot_wrap // 1000) & 0xFFFFFFFF)
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertEqual(q.read32(BASEBAND + 0x100), 0)
        self.assertEqual(q.read32(BASEBAND + 0x104), 624)
        self.assertEqual(q.read32(BASEBAND + 0x9C4), 0)

    def test_target3_exact_deadline_status_protection_and_ack(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, TARGET3)
        q.write32(BASEBAND + 0xF8, 1)
        # An incomplete initial pair must not create a reset-default event.
        q.command("clock_step 500")
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        q.write32(BASEBAND + 0xFC, 622)  # 312500 + 2 * 500 ns
        q.command("clock_step 312999")
        self.assertFalse(q.irq(1))
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        q.command("clock_step 1")
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        self.assertTrue(q.irq(1))
        self.assertFalse(q.irq(0))
        # Neither a status write nor acknowledgment of another source clears it.
        q.write32(BASEBAND + 0x1C, 0)
        q.write32(BASEBAND + 0x20, 1)
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        q.write32(BASEBAND + 0x20, TARGET3)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))
        # The equality still holds in this fine interval; ACK does not rearm.
        q.command("clock_step 1")
        q.write32(BASEBAND + 0x18, 0)
        q.write32(BASEBAND + 0x18, TARGET3)
        q.write32(BASEBAND + 0x1C, TARGET3)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))

    def test_target3_programming_phase_uses_epoch_and_current_interval_matches(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.command("clock_step 501")
        q.write32(BASEBAND + 0x18, TARGET3)
        q.write32(BASEBAND + 0xF8, 0)
        q.write32(BASEBAND + 0xFC, 620)  # 2000 ns, independent of write phase.
        q.command("clock_step 1498")
        self.assertFalse(q.irq(1))
        q.command("clock_step 1")
        self.assertTrue(q.irq(1))
        q.write32(BASEBAND + 0x20, TARGET3)
        q.command("clock_step 1")
        q.write32(BASEBAND + 0xFC, 620)  # Exact match within the current cell.
        q.command("clock_step 1")
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        self.assertTrue(q.irq(1))
        q.write32(BASEBAND + 0x20, TARGET3)
        q.command("clock_step 200")
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))

    def test_target3_mask_gates_status_and_irq_but_preserves_pending(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0xF8, 2)
        q.write32(BASEBAND + 0xFC, 624)
        q.command("clock_step 625000")
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))
        q.write32(BASEBAND + 0x18, TARGET3)
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        self.assertTrue(q.irq(1))
        q.write32(BASEBAND + 0x18, 0)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))
        q.write32(BASEBAND + 0x18, TARGET3)
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        self.assertTrue(q.irq(1))
        q.write32(BASEBAND + 0x20, TARGET3)
        q.write32(BASEBAND + 0xF8, 3)
        q.write32(BASEBAND + 0xFC, 624)
        q.write32(BASEBAND + 0x18, 0)  # Original cancellation only masks.
        q.command("clock_step 312500")
        q.write32(BASEBAND + 0x20, TARGET3)  # ACK also clears hidden pending.
        q.write32(BASEBAND + 0x18, TARGET3)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))
        self.assertFalse(q.irq(0))

    def test_target3_reprogram_replaces_deadline_and_invalid_fields_preserve_it(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, TARGET3)
        q.write32(BASEBAND + 0xF8, 2)
        q.write32(BASEBAND + 0xFC, 624)
        q.command("clock_step 1000")
        q.write32(BASEBAND + 0xF8, 4)
        q.write32(BASEBAND + 0xFC, 0)  # New deadline: 1562000 ns.
        for offset, invalid in ((0xF8, 0x10000000), (0xFC, 625), (0xFC, 0x400)):
            q.write32(BASEBAND + offset, invalid)
        self.assertEqual(q.read32(BASEBAND + 0xF8), 4)
        self.assertEqual(q.read32(BASEBAND + 0xFC), 0)
        q.command("clock_step 624000")
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        q.command("clock_step 936999")
        self.assertFalse(q.irq(1))
        q.command("clock_step 1")
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        self.assertTrue(q.irq(1))
        # Reprogramming retains already-latched pending until the driver ACKs.
        q.write32(BASEBAND + 0xF8, 8)
        q.write32(BASEBAND + 0xFC, 624)
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        q.write32(BASEBAND + 0x20, TARGET3)
        self.assertFalse(q.irq(1))
        q.command("clock_step 938000")
        self.assertTrue(q.irq(1))

    def test_target3_halfslot_wrap_and_fine_boundaries(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        slot_wrap = (1 << 28) * 312500
        q.command(f"clock_step {slot_wrap - 1000}")
        q.write32(BASEBAND + 0x18, TARGET3)
        # Both field orders work; neither initially incomplete pair can fire.
        q.write32(BASEBAND + 0xFC, 624)
        q.write32(BASEBAND + 0xF8, 0)
        q.command("clock_step 999")
        self.assertFalse(q.irq(1))
        q.command("clock_step 1")
        self.assertTrue(q.irq(1))
        q.write32(BASEBAND + 0x20, TARGET3)
        q.write32(BASEBAND + 0xFC, 0)
        q.command("clock_step 311999")
        self.assertFalse(q.irq(1))
        q.command("clock_step 1")
        self.assertEqual(q.read32(BASEBAND + 0x1C), TARGET3)
        self.assertTrue(q.irq(1))
        self.assertFalse(q.irq(0))

    def test_target3_past_pair_is_retained_and_later_future_write_recovers(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.command("clock_step 1000")
        q.write32(BASEBAND + 0x18, TARGET3)
        q.write32(BASEBAND + 0xF8, 0)
        q.write32(BASEBAND + 0xFC, 624)
        self.assertEqual(q.read32(BASEBAND + 0xFC), 624)
        q.command("clock_step 500")
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))
        self.assertIn("target3 past/ambiguous target is not armed",
                      q.stderr_path.read_text(errors="replace"))
        q.write32(BASEBAND + 0xFC, 620)  # Slot 0, elapsed 4 half-us.
        q.command("clock_step 499")
        self.assertFalse(q.irq(1))
        q.command("clock_step 1")
        self.assertTrue(q.irq(1))

    def test_target3_reset_cancels_deadline_and_clears_irq(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, TARGET3)
        q.write32(BASEBAND + 0xF8, 1)
        q.write32(BASEBAND + 0xFC, 624)
        q.write32(BASEBAND, 0x80000000)
        q.command("clock_step 312500")
        self.assertEqual(q.read32(BASEBAND + 0x18), TARGET3)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))
        # Reset keeps the running clock but requires a new complete target pair.
        q.write32(BASEBAND + 0xF8, 2)
        q.write32(BASEBAND + 0xFC, 624)
        q.command("clock_step 312500")
        self.assertTrue(q.irq(1))
        q.write32(BASEBAND, 0x80000000)
        self.assertFalse(q.irq(1))
        q.write32(BASEBAND + 0xF8, 3)
        q.write32(BASEBAND + 0xFC, 624)
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertEqual(q.read32(BASEBAND + 0x18), 0)
        self.assertEqual(q.read32(BASEBAND + 0xF8), 0)
        self.assertEqual(q.read32(BASEBAND + 0xFC), 0)
        q.write32(BASEBAND + 0x18, TARGET3)
        q.command("clock_step 1000000")
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(1))

    def test_event_table_and_wakeup_configuration_has_no_operational_effects(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(0x68006000, 0x12345678)
        # These configure a future scheduler or sleep transition. They neither
        # access an event table nor assert completion/sleep status on their own.
        values = {0x2C: 0x1234, 0x30: 0x80000000,
                  0x3C: (102 << 21) | (102 << 10) | 26,
                  0xE0: (280 << 16) | 200}
        for offset, value in values.items():
            q.write32(BASEBAND + offset, value)
        q.command("clock_step 100000000")
        for offset, value in values.items():
            self.assertEqual(q.read32(BASEBAND + offset), value)
        self.assertEqual(q.read32(0x68006000), 0x12345678)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertFalse(q.irq(0))
        self.assertFalse(q.irq(1))
        self.assertNotIn("baseband unsupported",
                         q.stderr_path.read_text(errors="replace"))

        # Unsupported fields reject the whole transaction and preserve config.
        for offset, invalid in ((0x2C, 0x4000), (0x30, 0x10), (0x30, 0x100),
                                (0x30, 4), (0x30, 8),
                                (0xE0, 0x200), (0xE0, 0x04000000)):
            q.write32(BASEBAND + offset, values[offset] | invalid)
            self.assertEqual(q.read32(BASEBAND + offset), values[offset])
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset in values:
            self.assertEqual(q.read32(BASEBAND + offset), 0)

    def test_machine_reset_restores_documented_configuration_defaults(self):
        q = self.machine()
        defaults = {0x64: 0, 0xC04: 0, 0xC80: 1, 0xC88: 0x3B8F,
                    0xC8C: 0x57FC0134,
                    0xC90: 0x1A1A0000, 0xCBC: 0xA, 0xD54: 0}
        for offset, expected in defaults.items():
            self.assertEqual(q.read32(BASEBAND + offset), expected)
            q.write32(BASEBAND + offset, 0)
        q.write32(BASEBAND + 0x64, 0xFFFFFFFF)
        q.write32(BASEBAND + 0x18, 0x800E)
        q.qmp("system_reset")
        q.qmp("query-status")
        for offset, expected in defaults.items():
            self.assertEqual(q.read32(BASEBAND + offset), expected)
        self.assertEqual(q.read32(BASEBAND + 0x18), 0)

    def test_btc_guest_target3_wakes_wfi_and_returns_from_irq1(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    cpsid i
    ldr r0, =0xe000e100
    movs r1, #2
    str r1, [r0]
    ldr r0, =0x6d040000
    mov r1, #0x80000000
    str r1, [r0, #0x100]
    ldr r1, [r0, #0x100]
    adds r1, r1, #32
    bic r1, r1, #0xf0000000
    str r1, [r0, #0xf8]
    movw r1, #624
    str r1, [r0, #0xfc]
    movs r1, #0x80
    str r1, [r0, #0x20]
    str r1, [r0, #0x18]
    ldr r4, =0x68001000
    cpsie i
1:  ldr r1, [r4, #4]
    cmp r1, #0
    bne 2f
    wfi
    b 1b
2:  movs r1, #0xd4
    str r1, [r4]
3:  wfi
    b 3b
    .ltorg
.org 0x100
.thumb_func
target3_irq:
    ldr r0, =0x6d040000
    ldr r1, [r0, #0x1c]
    ldr r2, =0x68001000
    str r1, [r2, #8]
    mrs r1, ipsr
    str r1, [r2, #12]
    movs r1, #0x80
    str r1, [r0, #0x20]
    ldr r1, [r2, #4]
    adds r1, r1, #1
    str r1, [r2, #4]
    bx lr
    .ltorg
""", base=0x68004000)
        q = self.machine(tcg=True)
        vectors = bytearray(0x100)
        struct.pack_into("<II", vectors, 0, 0x68017F00, 0x68004001)
        struct.pack_into("<I", vectors, (16 + 1) * 4, 0x68004101)
        q.write(0x68002000, vectors)
        q.write(0x68004000, code)
        q.write32(0x40080168, 0x68002000)
        q.qmp("cont")
        q.write32(0x400800CC, 0x88)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if q.read32(0x68001000) == 0xD4:
                break
            time.sleep(0.01)
        else:
            self.fail(q.diagnostic("BTC target3 IRQ1 guest did not return"))
        q.qmp("stop")
        self.assertEqual(struct.unpack("<4I", q.read(0x68001000, 16)),
                         (0xD4, 1, TARGET3, 17))
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertNotIn("Lockup", q.stderr_path.read_text(errors="replace"))

    def test_btc_guest_software_request_enters_irq1_and_returns(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    cpsid i
    ldr r0, =0xe000e100
    movs r1, #2
    str r1, [r0]
    ldr r0, =0x6d040000
    movs r1, #8
    str r1, [r0, #0x20]
    str r1, [r0, #0x18]
    ldr r1, [r0]
    orr r1, r1, #0x08000000
    str r1, [r0]
    cpsie i
    ldr r4, =0x68001000
1:  ldr r1, [r4, #4]
    cmp r1, #0
    beq 1b
    movs r1, #0xd5
    str r1, [r4]
2:  wfi
    b 2b
    .ltorg
.org 0x100
.thumb_func
software_irq:
    ldr r0, =0x6d040000
    ldr r1, [r0, #0x1c]
    ldr r2, =0x68001000
    str r1, [r2, #8]
    mrs r1, ipsr
    str r1, [r2, #12]
    movs r1, #8
    str r1, [r0, #0x20]
    ldr r1, [r2, #4]
    adds r1, r1, #1
    str r1, [r2, #4]
    bx lr
    .ltorg
""", base=0x68004000)
        q = self.machine(tcg=True)
        vectors = bytearray(0x100)
        struct.pack_into('<II', vectors, 0, 0x68017f00, 0x68004001)
        struct.pack_into('<I', vectors, 17 * 4, 0x68004101)
        q.write(0x68002000, vectors)
        q.write(0x68004000, code)
        q.write32(0x40080168, 0x68002000)
        q.qmp('cont')
        q.write32(0x400800cc, 0x88)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if q.read32(0x68001000) == 0xd5:
                break
            time.sleep(.01)
        else:
            self.fail(q.diagnostic('BTC software request did not return from IRQ1'))
        q.qmp('stop')
        self.assertEqual(struct.unpack('<4I', q.read(0x68001000, 16)),
                         (0xd5, 1, 8, 17))
        self.assertEqual(q.read32(BASEBAND + 0x1c), 0)
        self.assertEqual(q.read32(BASEBAND), 0)
        self.assertNotIn('baseband unsupported', q.stderr_path.read_text(errors='replace'))

    def test_btc_guest_profile_rmw_and_reset_poll_share_the_machine_view(self):
        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r0, =0x6d040000
    ldr r1, [r0, #0x64]
    bic r1, r1, #0x0000ff00
    orr r1, r1, #0x00005a00
    str r1, [r0, #0x64]
    ldr r1, [r0]
    orr r1, r1, #0x80000000
    str r1, [r0]
1:  ldr r1, [r0]
    lsrs r1, r1, #31
    bne 1b
    str r1, [r0, #0x2c]
    movw r1, #0x800e
    str r1, [r0, #0x18]
    mov r1, #-1
    str r1, [r0, #0x20]
    ldr r1, =0x0cc1981a
    str r1, [r0, #0x3c]
    ldr r1, [r0, #0x30]
    orr r1, r1, #0x80000000
    str r1, [r0, #0x30]
    ldr r1, =0x011800c8
    str r1, [r0, #0xe0]
    ldr r1, [r0, #0x100]
    orr r1, r1, #0x80000000
    str r1, [r0, #0x100]
3:  ldr r1, [r0, #0x100]
    cmp r1, #0
    blt 3b
    ldr r2, =0x68001004
    str r1, [r2]
    ldr r1, [r0, #0x104]
    str r1, [r2, #4]
    ldr r1, [r0, #0x9c4]
    str r1, [r2, #8]
    ldr r0, =0x68001000
    movs r1, #0x51
    str r1, [r0]
2:  wfi
    b 2b
    .ltorg
""", base=0x68004000)
        q = self.machine(tcg=True)
        vectors = struct.pack("<II", 0x68017F00, 0x68004001) + bytes(0xF8)
        q.write(0x68002000, vectors)
        q.write(0x68004000, code)
        q.write32(BASEBAND + 0x64, 0x12345678)
        q.write32(0x40080168, 0x68002000)
        q.qmp("cont")
        q.write32(0x400800CC, 0x88)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if q.read32(0x68001000) == 0x51:
                break
            time.sleep(0.01)
        else:
            self.fail(q.diagnostic("BTC reset-poll guest did not finish"))
        q.qmp("stop")
        self.assertEqual(q.read32(BASEBAND + 0x64), 0x12345A78)
        self.assertEqual(q.read32(BASEBAND + 0x18), 0x800E)
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)
        self.assertEqual(q.read32(BASEBAND + 0x2C), 0)
        self.assertEqual(q.read32(BASEBAND + 0x30), 0x80000000)
        self.assertEqual(q.read32(BASEBAND + 0x3C), 0x0CC1981A)
        self.assertEqual(q.read32(BASEBAND + 0xE0), 0x011800C8)
        for index, offset in enumerate((0x100, 0x104, 0x9C4)):
            self.assertEqual(q.read32(0x68001004 + index * 4),
                             q.read32(BASEBAND + offset))
        self.assertLessEqual(q.read32(BASEBAND + 0x104), 624)
        self.assertNotIn("Lockup", q.stderr_path.read_text(errors="replace"))


    def test_extended_register_fields_and_capture_latches(self):
        q = self.machine()
        # These masks and addresses come from the BEST1700 driver accessors.
        values = {
            0x614: 0x00805555, 0x618: 0x68005E58, 0x624: 0x00008010,
            0x634: 0x00805555, 0x670: 0x80, 0x674: 0,
            0x688: 0x80AA01FF, 0xC00: 0xA5A5A5A5,
            0xC4C: 0x03020100, 0xC60: 0x00805533,
            0xC64: 0x00AA1022, 0xC68: 0x007F2033,
            0xC6C: 0x00504044, 0xD08: 0x00123456,
            0xD0C: 0x80000201, 0xD10: 0x00234567,
            0xD14: 0x80000102, 0xD18: 0x00345678,
            0xD1C: 0x80000103, 0xD28: 0x80000000,
            0xD7C: 0x02112233, 0xD80: 0x00456789,
            0xD84: 0x80000104, 0xD88: 0x0056789A,
            0xD8C: 0x80000105, 0xE60: 0x00123480,
        }
        for offset, value in values.items():
            q.write32(BASEBAND + offset, value)
            self.assertEqual(q.read32(BASEBAND + offset), value,
                             f"BTC register 0x{offset:x}")
        # The current-clock register advances with virtual time and is
        # separate from the DMA/GPIO capture latches.
        before = q.read32(BASEBAND + 0xE04)
        q.step(625 * 500)
        self.assertGreaterEqual(q.read32(BASEBAND + 0xE04), before)
        for offset in (0xE00, 0xE08, 0xE1C, 0xE20, 0xE24, 0xE28,
                       0xE2C, 0xE30, 0xE34, 0xE38, 0xE3C, 0xE40,
                       0xE44, 0xE48, 0xE4C, 0xE50):
            self.assertEqual(q.read32(BASEBAND + offset), 0)
            q.write32(BASEBAND + offset, 0xFFFFFFFF)
            self.assertEqual(q.read32(BASEBAND + offset), 0)
        log = q.stderr_path.read_text(errors="replace")
        # Capture latches are read-only hardware state. Software writes are
        # ignored by the block and must not become guest bus faults.
        self.assertNotIn("unsupported write offset=0xe00", log)
        self.assertNotIn("unsupported write offset=0xe50", log)

    def test_dma_terminal_count_captures_btc_clock(self):
        q = self.machine()
        q.write(0x68001000, b"Z")
        q.write32(BASEBAND + 0xC50, 0x80000001)
        q.write32(BASEBAND + 0xD70, 1 << 4)
        dma = 0x6D010000
        q.write32(dma + 0x30, 1)
        q.write32(dma + 0x100, 0x68001000)
        q.write32(dma + 0x104, 0x68001010)
        q.write32(dma + 0x10c, (1 << 31) | (1 << 27) | (1 << 26) | 1)
        q.write32(dma + 0x110, (1 << 15) | 1)
        q.step(1000)
        self.assertEqual(q.read(0x68001010, 1), b"Z")
        self.assertEqual(q.read32(dma + 0x14), 1)
        self.assertEqual(q.read32(BASEBAND + 0xE34), 0)
        self.assertLessEqual(q.read32(BASEBAND + 0xE38), 624)

    def test_bth_dma_terminal_count_captures_bth_slots(self):
        q = self.machine(tcg=True)
        # C4C selects BTH AUDMA/GPDMA source IDs. D70 bits 4 and 5
        # enable CAP0 and CAP1; C50 bit31 is the shared trigger gate.
        q.write32(BASEBAND + 0xC4C, 34 | (28 << 8))
        q.write32(BASEBAND + 0xC50, 1 << 31)
        q.write32(BASEBAND + 0xD70, (1 << 4) | (1 << 5))

        code = self.assemble(r"""
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
.thumb_func
reset_a:
    ldr r4, =0x23001000
    movs r0, #0x5a
    strb r0, [r4]
    ldr r5, =0x50170000
    movs r0, #1
    str r0, [r5, #0x30]
    str r4, [r5, #0x100]
    ldr r0, =0x23001020
    str r0, [r5, #0x104]
    movs r0, #0
    str r0, [r5, #0x108]
    ldr r0, =0x8c000001
    str r0, [r5, #0x10c]
    ldr r0, =0x00008001
    str r0, [r5, #0x110]
1:
    ldr r0, [r5, #0x14]
    tst r0, #1
    beq 1b
    ldr r5, =0x50130000
    movs r0, #1
    str r0, [r5, #0x30]
    str r4, [r5, #0x100]
    ldr r0, =0x23001024
    str r0, [r5, #0x104]
    movs r0, #0
    str r0, [r5, #0x108]
    ldr r0, =0x8c000001
    str r0, [r5, #0x10c]
    ldr r0, =0x00008001
    str r0, [r5, #0x110]
2:
    ldr r0, [r5, #0x14]
    tst r0, #1
    beq 2b
    ldr r0, =0x0000a1b2
    ldr r1, =0x23001030
    str r0, [r1]
3:
    wfi
    b 3b
    .ltorg
""", base=0x23004000)
        q.write(0x23004000, code)
        q.write32(0x23002000, 0x230bff00)
        q.write32(0x23002004, 0x23004001)
        q.write32(0x400800b8, 0x23002000)
        q.write32(0x400800cc, 0x300)
        q.qmp("cont")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if q.read32(0x23001030) == 0x0000a1b2:
                break
            time.sleep(0.01)
        else:
            self.fail(q.diagnostic("BTH DMA terminal count did not complete"))
        q.qmp("stop")
        self.assertEqual(q.read(0x23001020, 1), b"\x5a")
        self.assertEqual(q.read(0x23001024, 1), b"\x5a")
        self.assertGreaterEqual(q.read32(BASEBAND + 0xE00), 0)
        self.assertGreaterEqual(q.read32(BASEBAND + 0xE1C), 0)
        self.assertLessEqual(q.read32(BASEBAND + 0xE08), 624)
        self.assertLessEqual(q.read32(BASEBAND + 0xE20), 624)

    def test_software_trigger_routes_arm_and_disable(self):
        q = self.machine()
        q.command("irq_intercept_in /machine/btc-core")
        q.write32(BASEBAND + 0x18, 1 << 5)
        q.write32(BASEBAND + 0xC94, 1)
        q.write32(BASEBAND + 0xC98, (1 << 31) | 624)
        q.step(312_500 - 1)
        self.assertFalse(q.irq(1))
        q.step(1)
        self.assertTrue(q.irq(1))
        self.assertEqual(q.read32(BASEBAND + 0x1C), 1 << 5)
        q.write32(BASEBAND + 0x20, 1 << 5)
        q.write32(BASEBAND + 0xC98, 624)
        q.write32(BASEBAND + 0xC94, 2)
        q.step(312_500)
        self.assertFalse(q.irq(1))
        self.assertEqual(q.read32(BASEBAND + 0x1C), 0)


if __name__ == "__main__":
    unittest.main()
