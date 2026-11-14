"""HP4560X bus contract, with a synthetic BEST1503 guest and board."""
import json
import os
from pathlib import Path
import tempfile
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine
from tests import test_sc7061_qtest as sc7061
from tools.synthetic_config import configuration


@unittest.skipUnless(os.environ.get('MIBAND_TEST_QEMU'), 'MIBAND_TEST_QEMU is not set')
class Hp4560x(unittest.TestCase):
    read_reg = sc7061.ChargerIdentity.read_reg
    write_reg = sc7061.ChargerIdentity.write_reg
    cable = sc7061.ChargerIdentity.cable

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='hp4560x-qtest-')
        self.addCleanup(temp.cleanup)
        config = Path(temp.name) / 'hardware.json'
        config.write_text(json.dumps(configuration()), encoding='utf-8')
        self.q = QtestMachine(os.environ['MIBAND_TEST_QEMU'],
            machine_name='bes1503',
            machine_options=(',gpadc-model=on,charger-model=on,charger-kind=hp4560x'
                             ',hardware-config=' + str(config)),
            image_payload=bytes.fromhex('fee7'), serial_backends=('none', 'none'))
        self.addCleanup(self.q.close)
        self.enable_bus()

    def enable_bus(self):
        self.q.write32(0x40005004, 7)
        self.q.write32(0x4000506c, 1)

    def test_probe_configuration_and_reset(self):
        self.assertEqual(self.read_reg(0x0a), 0xe0)
        self.write_reg(0x0a, 0)
        self.assertEqual(self.read_reg(0x0a), 0xe0)
        # These are writable configuration fields, not SC7061 identity bytes.
        self.write_reg(1, 8)
        self.assertEqual(self.read_reg(1), 8)
        self.write_reg(6, 0x20)
        self.write_reg(0x1b, 2)
        self.assertEqual(self.read_reg(6), 0x20)
        self.assertEqual(self.read_reg(0x1b), 2)
        self.cable(True)
        self.write_reg(2, 0x80)
        self.assertEqual(self.read_reg(2) & 0x80, 0)
        self.assertEqual(self.read_reg(1), 0)
        self.assertEqual(self.read_reg(0x0a), 0xe0)
        self.assertEqual(self.read_reg(8), 6)
        self.q.qmp('system_reset')
        self.enable_bus()
        self.assertEqual(self.read_reg(0x0a), 0xe0)
        self.assertEqual(self.read_reg(8), 6)
        with self.assertRaisesRegex(RuntimeError, 'startup-only'):
            self.q.qmp('qom-set', {'path': '/machine', 'property': 'charger-kind',
                                   'value': 'sc7061'})

    def test_cable_and_readonly_fault_status(self):
        self.assertEqual(self.read_reg(8), 0)
        self.cable(True)
        self.assertEqual(self.read_reg(8), 6)
        self.write_reg(8, 0xff)
        self.write_reg(9, 0xff)
        self.assertEqual(self.read_reg(8), 6)
        self.assertEqual(self.read_reg(9), 0)
        self.cable(False)
        self.assertEqual(self.read_reg(8), 0)
        self.assertEqual(self.read_reg(0x40), 0, 'no SC7061 IRQ register')


if __name__ == '__main__':
    unittest.main()
