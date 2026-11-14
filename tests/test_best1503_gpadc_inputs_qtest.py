"""Static board voltages through the real PMU ISPI ADC register interface."""
import json
import os
from pathlib import Path
import tempfile
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine
from tools.synthetic_config import configuration


ISPI = 0x40009000
DATA = 0x56


@unittest.skipUnless(os.environ.get('MIBAND_TEST_QEMU'), 'built QEMU required')
class GpadcInputs(unittest.TestCase):
    def machine(self, options):
        q = QtestMachine(os.environ['MIBAND_TEST_QEMU'],
            machine_name='bes1503', machine_options=options,
            image_payload=bytes.fromhex('fee7'), serial_backends=('none', 'none'))
        self.addCleanup(q.close)
        return q

    def read(self, q, reg):
        q.write32(ISPI + 4, 3 << 7)
        q.write32(ISPI + 8, (1 << 26) | (reg << 18))
        return q.read32(ISPI + 8) >> 2

    def test_default_and_sentinel_keep_existing_samples(self):
        q = self.machine(',gpadc-model=on,gpadc-mv4=4294967295')
        for channel in range(7):
            self.assertEqual(self.read(q, DATA + channel), 0x6800)
        self.assertEqual(self.read(q, DATA + 15), 0x7c00)

    def test_voltage_channels_are_isolated_and_survive_reset(self):
        q = self.machine(',gpadc-model=on,gpadc-mv0=0,gpadc-mv4=1000,gpadc-mv6=1800')
        for _ in range(2):
            samples = [self.read(q, DATA + channel) for channel in range(7)]
            for channel, mv in ((0, 0), (4, 1000), (6, 1800)):
                # Default model calibration is 15968 at800mV,20816 at1050mV.
                # Reconstruct voltage from the bus sample, allowing rounding.
                measured = (samples[channel] - 15968) * 250 / 4848 + 800
                self.assertAlmostEqual(measured, mv, delta=1)
            for channel in (1, 2, 3, 5):
                self.assertEqual(samples[channel], 0x6800)
            q.qmp('system_reset')

    def test_live_rewiring_is_rejected(self):
        q = self.machine(',gpadc-model=on,gpadc-mv4=1000')
        before = self.read(q, DATA + 4)
        with self.assertRaisesRegex(RuntimeError, 'startup-only'):
            q.qmp('qom-set', dict(path='/machine', property='gpadc-mv4', value=800))
        self.assertEqual(self.read(q, DATA + 4), before)

    def test_invalid_voltage_and_disabled_model_fail_startup(self):
        for options, message in (
                (',gpadc-model=on,gpadc-mv4=1801', '0..1800 mV'),
                (',gpadc-model=on,gpadc-mv4=4294967294', '0..1800 mV'),
                (',gpadc-mv4=1000', 'requires gpadc-model=on')):
            with self.subTest(options=options):
                q = self.machine(options)
                self.assertNotEqual(q.process.wait(timeout=5), 0)
                self.assertIn(message, q.stderr_path.read_text(errors='replace'))

    def test_power_channels_cannot_be_overridden(self):
        temp = tempfile.TemporaryDirectory(prefix='gpadc-inputs-')
        self.addCleanup(temp.cleanup)
        path = Path(temp.name) / 'hardware.json'
        path.write_text(json.dumps(configuration()), encoding='utf-8')
        base = ',gpadc-model=on,charger-model=on,hardware-config=' + str(path)
        for channel in (0, 1):
            with self.subTest(channel=channel):
                q = self.machine(base + f',gpadc-mv{channel}=1000')
                self.assertNotEqual(q.process.wait(timeout=5), 0)
                self.assertIn('overlaps board power input',
                              q.stderr_path.read_text(errors='replace'))
        q = self.machine(base + ',gpadc-mv4=1000')
        before = self.read(q, DATA + 4)
        q.qmp('qom-set', dict(path='/machine', property='charger-state',
                              value=dict(level=0, charging=False)))
        self.assertEqual(self.read(q, DATA + 4), before)
        self.assertNotEqual(self.read(q, DATA), 0x6800)


if __name__ == '__main__':
    unittest.main()
