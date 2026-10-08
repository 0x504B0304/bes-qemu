"""CP playback mode selection is configuration, never a trigger completion."""
import os
import unittest

from tests import test_best1700_btc_baseband_qtest as baseband_qtest


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class CpPlaybackTests(unittest.TestCase):
    machine = baseband_qtest.BtcBasebandTests.machine

    def test_cp_software_mode_rmw_preserves_configuration_without_trigger(self):
        q = self.machine()
        base = 0x6d040000
        q.command('irq_intercept_in /machine/btc-core')
        q.write32(base + 0x18, 0x80ff)
        q.write32(base + 0xc7c, 0x03300000)
        # Original CP 1b361c96..1b361ca2 selects playback mode 1.
        q.write32(base + 0xc7c,
                  (q.read32(base + 0xc7c) & 0xffff9fff) | (1 << 13))
        self.assertEqual(q.read32(base + 0xc7c), 0x03302000)
        for invalid in (0x03302002, 0x03302020, 0x03302080,
                        0x03b02000, 0x03702000, 0x43302000,
                        0x83302000):
            q.write32(base + 0xc7c, invalid)
            self.assertEqual(q.read32(base + 0xc7c), 0x03302000)
        q.command('clock_step 100000000')
        self.assertEqual(q.read32(base + 0x1c), 0)
        self.assertEqual(q.read32(base + 0xc94), 0)
        self.assertEqual(q.read32(base + 0xc98), 0)
        self.assertFalse(q.irq(1))
        q.write32(base + 0xc7c, q.read32(base + 0xc7c) & ~0x6000)
        self.assertEqual(q.read32(base + 0xc7c), 0x03300000)
