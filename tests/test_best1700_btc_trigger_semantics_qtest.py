import os
import unittest
from tests import test_best1700_btc_baseband_qtest as baseband_qtest


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class BtcTriggerSemanticsTests(unittest.TestCase):
    machine = baseband_qtest.BtcBasebandTests.machine

    def test_c7c_playback_fields_and_route0_lifecycle(self):
        q = self.machine()
        base = 0x6D040000
        q.command('irq_intercept_in /machine/btc-core')
        q.write32(base + 0x18, 0x80ff)

        # Route 0 uses the C94/C98 timestamp pair.  C98 bit 31 and CNTLX
        # bit 0 are two views of the same hardware enable latch.
        q.write32(base + 0xc94, 20)
        q.write32(base + 0xc98, 0x80000000)
        self.assertTrue(q.read32(base + 0xc7c) & 1)
        self.assertTrue(q.read32(base + 0xe60) & 0x80)

        fields = 1 | (1 << 3) | (1 << 8) | (2 << 13) | (1 << 15) | (5 << 16)
        q.write32(base + 0xc7c, fields)
        self.assertEqual(q.read32(base + 0xc7c), fields)
        self.assertTrue(q.read32(base + 0xe60) & 0x80)

        # The armed software route raises the route-0 interrupt at its virtual
        # timestamp and does not synthesize a radio completion event.
        q.command('clock_step 10000000')
        self.assertTrue(q.read32(base + 0x1c) & (1 << 5))
        self.assertTrue(q.irq(1))

        # CNTL1 route clear is write-one/self-clearing and clears both views.
        q.write32(base + 0xc08, 1 << 4)
        self.assertFalse(q.read32(base + 0xc7c) & 1)
        self.assertFalse(q.read32(base + 0xc98) & 0x80000000)
        self.assertFalse(q.read32(base + 0xe60) & 0x80)

    def test_c08_command_bits_do_not_latch_as_configuration(self):
        q = self.machine()
        base = 0x6D040000
        q.write32(base + 0x18, 0x80ff)
        before = q.read32(base + 0xc08)
        q.write32(base + 0xc08, (1 << 1) | (1 << 2) | (1 << 3) |
                  (1 << 5) | (1 << 6) | (1 << 7) | (1 << 11) |
                  (1 << 12) | (1 << 13))
        self.assertEqual(q.read32(base + 0xc08), before)
