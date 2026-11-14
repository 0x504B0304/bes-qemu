"""BEST1503 PMU soft poweroff, distinct from GPADC and reset fallback."""
import json
import os
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine

ISPI = 0x40009000


@unittest.skipUnless(os.environ.get('MIBAND_TEST_QEMU'), 'built QEMU required')
class Best1503Poweroff(unittest.TestCase):
    def setUp(self):
        self.q = QtestMachine(os.environ['MIBAND_TEST_QEMU'],
            machine_name='bes1503', machine_options=',gpadc-model=on',
            image_payload=bytes.fromhex('fee7'), serial_backends=('none', 'none'),
            extra_args=('-action', 'shutdown=pause'))
        self.addCleanup(self.q.close)
        self.q.qmp('query-status')  # Subscribe before power removal.

    def write(self, reg, value, cs=3):
        self.q.write32(ISPI + 4, cs << 7)
        self.q.write32(ISPI + 8, (reg << 18) | (value << 2))
        self.q.read32(ISPI + 8)

    def read(self, reg):
        self.q.write32(ISPI + 4, 3 << 7)
        self.q.write32(ISPI + 8, (1 << 26) | (reg << 18))
        return self.q.read32(ISPI + 8) >> 2

    def shutdown(self):
        self.q.qmp_socket.settimeout(3)
        while True:
            event = json.loads(self.q.qmp_stream.readline())
            self.assertNotEqual(event.get('event'), 'RESET')
            if event.get('event') == 'SHUTDOWN':
                self.assertEqual(event['data']['reason'], 'guest-shutdown')
                self.assertIs(event['data']['guest'], True)
                break
        self.assertEqual(self.q.qmp('query-status')['status'], 'shutdown')
        with self.assertRaises(RuntimeError):
            self.q.qmp('cont')

    def test_soft_poweroff_works_without_charger_or_boot_adapter(self):
        self.write(0x4f, 1)
        self.shutdown()

    def test_adc_page_and_other_selects_do_not_remove_power(self):
        self.write(0x1d, 1)
        self.write(0x20, 1)
        for value in (0, 0x20, 0x40, 0x60):
            self.write(0x4f, value)
            self.assertTrue(self.q.qmp('query-status')['running'])
        self.assertEqual(self.read(0x50), 1, 'GPADC is still functional')
        self.write(0, 0xa010)
        self.write(0x4f, 1)
        self.write(0, 0xa000)
        self.assertTrue(self.q.qmp('query-status')['running'])
        for cs in (0, 1, 2, 4, 15):
            self.write(0x4f, 1, cs=cs)
            self.assertTrue(self.q.qmp('query-status')['running'])
        self.write(0x4f, 0x21)
        self.shutdown()
        self.assertEqual(self.read(0x50), 1)

    def test_reset_clears_command_and_rearms_poweroff(self):
        for _ in range(2):
            self.write(0x4f, 1)
            self.shutdown()
            self.q.qmp('system_reset')
            self.assertEqual(self.read(0x4f) & 1, 0)
            self.q.qmp('cont')
            self.assertTrue(self.q.qmp('query-status')['running'])


if __name__ == '__main__':
    unittest.main()
