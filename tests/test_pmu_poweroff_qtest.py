"""Synthetic ISPI poweroff acceptance. Opt-in; no external firmware required."""
import json
import os
import unittest
from tests.test_best1700_peripherals_qtest import QtestMachine

ISPI = 0x4000a000


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'), 'built QEMU required')
class PmuPoweroff(unittest.TestCase):
    def machine(self):
        q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'], extra_args=(
            '-device', 'bes1806p-pmu,id=pmu,bus=/best1700-ispi/ispi,key-code=home',
            '-device', 'miband-guest-power,source=/machine/peripheral/pmu',
            '-action', 'shutdown=pause'))
        self.addCleanup(q.close)
        q.qmp('query-status')  # Subscribe before the write.
        return q

    def write(self, q, reg, value, *, normal=False, cs=0):
        q.write32(ISPI, (28 << 16) | 0x100 if normal else 24 << 16)
        q.write32(ISPI + 4, cs << 7)
        q.write32(ISPI + 8, (reg << 18) | (value << 2) if normal or cs else (reg << 16) | value)
        q.read32(ISPI + 8)

    def read(self, q, reg):
        q.write32(ISPI, 24 << 16)
        q.write32(ISPI + 4, 0)
        q.write32(ISPI + 8, (1 << 24) | (reg << 16))
        return q.read32(ISPI + 8)

    def expect_shutdown(self, q):
        q.qmp_socket.settimeout(3)
        while True:
            event = json.loads(q.qmp_stream.readline())
            if event.get('event') == 'SHUTDOWN':
                self.assertEqual(event['data']['reason'], 'guest-shutdown')
                self.assertIs(event['data']['guest'], True)
                break
        self.assertEqual(q.qmp('query-status')['status'], 'shutdown')
        with self.assertRaises(RuntimeError):
            q.qmp('cont')  # No implicit resume from poweroff.

    def test_external_frame_generates_standard_guest_shutdown(self):
        q = self.machine()
        self.write(q, 0x4f, 1)
        self.expect_shutdown(q)

    def test_normal_frame_generates_standard_guest_shutdown(self):
        q = self.machine()
        self.write(q, 0x4f, 1, normal=True)
        self.expect_shutdown(q)

    def test_bit6_adc_page_and_cs_do_not_poweroff_then_reset_rearms(self):
        q = self.machine()
        for _ in range(2):
            self.write(q, 0x27, 1)
            self.write(q, 0x4f, 0x60)  # GPADC_START plus variant-dependent bit6.
            self.assertEqual(self.read(q, 0x52), 1)
            self.write(q, 0x52, 1)
            self.assertEqual(self.read(q, 0x52), 1, 'ADC interval remains active')
            self.write(q, 0x4f, 0x40)
            self.write(q, 0x52, 1)
            self.assertEqual(self.read(q, 0x52), 0)
            self.assertEqual(self.read(q, 0x4f), 0x40)
            self.write(q, 0, 0xa010)
            self.write(q, 0x4f, 1)
            self.write(q, 0, 0xa000)
            self.write(q, 0x4f, 1, cs=1, normal=True)
            self.assertNotEqual(q.qmp('query-status')['status'], 'shutdown')
            self.write(q, 0x4f, 0x21)  # Both real side effects share the transaction.
            self.expect_shutdown(q)
            self.assertEqual(self.read(q, 0x52), 1)
            q.qmp('system_reset')
            self.assertEqual(self.read(q, 0), 0x18f0)
            self.assertEqual(self.read(q, 0x4f), 0)
            # QEMU reset leaves a powered-off VM in prelaunch. Explicitly
            # start it before testing a second power removal; vm_stop only
            # changes runstate while running. This is test setup, not a
            # supervisor implementation of power-on.
            self.assertEqual(q.qmp('query-status')['status'], 'prelaunch')
            q.qmp('cont')
            self.assertTrue(q.qmp('query-status')['running'])


if __name__ == '__main__':
    unittest.main()
