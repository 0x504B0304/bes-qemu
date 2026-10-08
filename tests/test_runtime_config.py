# SPDX-License-Identifier: GPL-2.0-or-later
"""Execute the public ABI against synthetic data and a real QEMU executable."""
import copy
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.synthetic_config import configuration


@unittest.skipUnless(os.environ.get('MIBAND_QEMU'), 'set MIBAND_QEMU to run ABI integration')
class RuntimeConfigTests(unittest.TestCase):
    def run_config(self, data, commands='quit\n', qmp=False, guest_bytes=None):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = Path(tmp)/'config.json'
            cfg.write_text(json.dumps(data))
            guest = Path(tmp)/'guest.bin'
            guest.write_bytes(guest_bytes or bytes.fromhex('fee7'))
            return subprocess.run([os.environ['MIBAND_QEMU'], '-M', f'bes1503,hardware-config={cfg}',
                '-kernel', str(guest), *(['-S'] if guest_bytes is None else
                    ['-semihosting-config', 'enable=on,target=native']),
                '-display', 'none', '-serial', 'null',
                *(['-qmp', 'stdio', '-monitor', 'none'] if qmp else
                  ['-monitor', 'none' if guest_bytes is not None else 'stdio'])],
                input=commands, text=True, capture_output=True, timeout=10)

    def test_independent_synthetic_board_and_startup_lock(self):
        cmds = [dict(execute='qmp_capabilities'),
            dict(execute='qom-get', arguments=dict(path='/machine', property='display-width')),
            dict(execute='qom-set', arguments=dict(path='/machine', property='hardware-config', value='absent')),
            dict(execute='quit')]
        result = self.run_config(configuration(), '\n'.join(map(json.dumps, cmds))+'\n', qmp=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('128', result.stdout)
        self.assertIn('startup-only', result.stdout)

    def test_synthetic_guest_reaches_semihost_exit_without_product_files(self):
        guest = bytes.fromhex('202001a1abbefee72600020000000000')
        result = self.run_config(configuration(), commands='', guest_bytes=guest)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_invalid_types_ranges_bindings_and_unknown_keys(self):
        mutations = [('abi', None, 3), ('board', 'width', 0), ('board', 'height', True),
                     ('board', 'touch_reset_gpio', 0), ('board', 'config_sha256', 'bad'),
                     ('board', 'unexpected', 1), ('firmware', 'board', 'other'),
                     ('firmware', 'mass_type_count', 2), ('firmware', 'ble_chunk_size', 22),
                     ('firmware', 'nor_size', 0x1000001), ('firmware', 'boot_pc', 0),
                     ('board', 'curve_uv', [4300000]*101)]
        for section, key, value in mutations:
            with self.subTest(section=section, key=key):
                data = copy.deepcopy(configuration())
                if key is None: data[section] = value
                else: data[section][key] = value
                result = self.run_config(data)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('hardware-config', result.stderr)

    def test_board_only_configuration_updates_machine_geometry(self):
        data = configuration()
        del data['firmware']
        data['board']['width'] = 96
        commands = [dict(execute='qmp_capabilities'), dict(execute='qom-get',
            arguments=dict(path='/machine', property='display-width')), dict(execute='quit')]
        result = self.run_config(data, '\n'.join(map(json.dumps, commands))+'\n', qmp=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('"return": 96', result.stdout)


if __name__ == '__main__':
    unittest.main()
