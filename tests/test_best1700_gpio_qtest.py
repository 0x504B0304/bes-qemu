"""GPIO interrupt banks and the board's physical panel TE connection."""
import os
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine

BANKS = (0x40081000, 0x40089000, 0x4008A000)
ENABLES = (0x400850FC, 0x400851A4, 0x400851C8)
STATUS = (0x40085108, 0x400851A8, 0x400851CC)
PULL_UP = (0x40086064, 0x40086068, 0x40086080)
PULL_DOWN = (0x4008606C, 0x40086070, 0x40086084)


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700GpioQtest(unittest.TestCase):
    def setUp(self):
        self.q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                              machine_options=',panel-te-pin=84')
        self.addCleanup(self.q.close)
        self.q.command('irq_intercept_in /machine/core[0]')

    def drive(self, pin, high):
        self.q.command(f'set_irq_in /machine/gpio input {pin} {int(high)}')

    def configure(self, pin, *, edge=True, rising=True):
        bank, bit = divmod(pin, 32)
        mask = 1 << bit
        q, base = self.q, BANKS[bank]
        q.write32(ENABLES[bank], mask)
        q.write32(base + (0x38 if edge else 0x20), mask)
        q.write32(base + (0x3C if rising else 0x24), mask)
        q.write32(base + 0x30, mask)  # INTEN_SET
        q.write32(base + 0x1C, mask)  # INTMASK_CLR
        return bank, base, mask

    def test_three_banks_have_independent_edge_latches_and_shared_irq(self):
        configured = [self.configure(pin) for pin in (5, 47, 84)]
        for pin, (bank, base, mask) in zip((5, 47, 84), configured):
            self.drive(pin, True)
            self.assertEqual(self.q.read32(base + 0x50) & mask, mask)
            self.drive(pin, False)
            self.assertEqual(self.q.read32(base + 0x44) & mask, mask)
            self.assertEqual(self.q.read32(base + 0x40) & mask, mask)
            self.assertEqual(self.q.read32(STATUS[bank]) & mask, mask)
            self.assertTrue(self.q.irq(2))
        for index, (_, base, mask) in enumerate(configured):
            self.q.write32(base + 0x4C, mask)
            self.assertEqual(self.q.read32(base + 0x44) & mask, 0)
            self.assertEqual(self.q.irq(2), index != 2)

    def test_mask_and_common_gate_preserve_pending_edge(self):
        bank, base, mask = self.configure(84)
        self.q.write32(base + 0x34, mask)
        self.drive(84, True)
        self.drive(84, False)
        self.assertEqual(self.q.read32(base + 0x44) & mask, mask)
        self.assertEqual(self.q.read32(base + 0x40) & mask, 0)
        self.assertFalse(self.q.irq(2))
        self.q.write32(ENABLES[bank], 0)
        self.q.write32(base + 0x1C, mask)
        self.assertFalse(self.q.irq(2))
        self.q.write32(ENABLES[bank], mask)
        self.assertTrue(self.q.irq(2))
        self.q.write32(base + 0x18, mask)
        self.assertFalse(self.q.irq(2))

    def test_level_interrupt_remains_active_until_input_deasserts(self):
        _, base, mask = self.configure(9, edge=False)
        self.drive(9, True)
        self.assertTrue(self.q.irq(2))
        self.q.write32(base + 0x4C, mask)
        self.assertTrue(self.q.irq(2))
        self.drive(9, False)
        self.assertFalse(self.q.irq(2))
        self.q.qmp('system_reset')
        self.q.qmp('query-status')
        self.assertFalse(self.q.irq(2))
        self.assertEqual(self.q.read32(ENABLES[0]), 0)

    def test_pad_bias_in_all_banks_and_external_signal_priority(self):
        for pin in (11, 43, 76):
            with self.subTest(pin=pin):
                bank, bit = divmod(pin, 32)
                mask, base = 1 << bit, BANKS[bank]
                self.assertEqual(self.q.read32(base + 0x50) & mask, 0)
                self.q.write32(PULL_UP[bank], mask)
                self.assertEqual(self.q.read32(PULL_UP[bank]), mask)
                self.assertEqual(self.q.read32(base + 0x50) & mask, mask)
                self.q.write32(PULL_UP[bank], 0)
                self.q.write32(PULL_DOWN[bank], mask)
                self.assertEqual(self.q.read32(base + 0x50) & mask, 0)
                self.q.write32(PULL_DOWN[bank], 0)
                self.q.write32(PULL_UP[bank], mask)
                self.drive(pin, False)
                self.assertEqual(self.q.read32(base + 0x50) & mask, 0)
                self.drive(pin, True)
                self.q.write32(PULL_DOWN[bank], mask)
                self.assertEqual(self.q.read32(base + 0x50) & mask, mask)
        self.q.qmp('system_reset')
        for up, down in zip(PULL_UP, PULL_DOWN):
            self.assertEqual(self.q.read32(up), 0)
            self.assertEqual(self.q.read32(down), 0)
        self.assertTrue(self.q.read32(BANKS[2] + 0x50) & (1 << 12),
                        'reset retains external input')

    def test_bias_changes_follow_edge_and_level_interrupt_configuration(self):
        bank, base, mask = self.configure(76, edge=True, rising=True)
        self.q.write32(PULL_UP[bank], mask)
        self.assertTrue(self.q.irq(2))
        self.q.write32(base + 0x4c, mask)
        self.assertFalse(self.q.irq(2))
        self.q.write32(base + 0x24, mask)  # Falling edge.
        self.q.write32(PULL_UP[bank], 0)
        self.assertTrue(self.q.irq(2))
        self.q.write32(base + 0x4c, mask)
        self.q.write32(base + 0x20, mask)  # Active-low level.
        self.q.write32(PULL_DOWN[bank], mask)
        self.assertTrue(self.q.irq(2))
        self.q.write32(PULL_DOWN[bank], 0)
        self.q.write32(PULL_UP[bank], mask)
        self.assertFalse(self.q.irq(2))

    def panel_command(self, command):
        self.q.write32(0x401D0018, (command << 8) | 0x05)
        self.q.write32(0x401D0014, 0x5E104)

    def test_panel_te_uses_configured_gpio_and_stops_when_disabled(self):
        _, base, mask = self.configure(84)
        self.panel_command(0x11)
        self.panel_command(0x29)
        self.q.step(20_000_000)
        self.assertFalse(self.q.irq(2))
        self.panel_command(0x35)
        self.q.step(20_000_000)
        self.assertTrue(self.q.irq(2))
        self.assertFalse(self.q.irq(28), 'TE is wired to GPIO, not DSI IRQ')
        self.q.write32(base + 0x4C, mask)
        self.assertFalse(self.q.irq(2))
        self.q.step(20_000_000)
        self.assertTrue(self.q.irq(2))
        self.panel_command(0x34)
        self.q.write32(base + 0x4C, mask)
        self.q.step(20_000_000)
        self.assertFalse(self.q.irq(2))


if __name__ == '__main__':
    unittest.main()
