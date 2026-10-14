# SPDX-License-Identifier: GPL-2.0-or-later
"""Public inline audio ABI, tested without proprietary firmware or helpers."""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from tools.synthetic_config import configuration


@unittest.skipUnless(os.environ.get('MIBAND_QEMU'), 'set MIBAND_QEMU')
class InlineAudioTests(unittest.TestCase):
    def fixture(self):
        guest = bytes.fromhex('00bf') * 32
        digest = hashlib.sha256(guest).hexdigest()
        base = 0x3e000000
        code = bytearray(106)
        struct.pack_into('<II', code, 0, 0x4150434d, 3)
        slots = (8, 12, 16, 20, 28, 32, 36)
        for slot in slots:
            struct.pack_into('<I', code, slot, base + 105)
        code[40:72] = bytes.fromhex(digest)
        code[72:104] = bytes.fromhex('11' * 32)
        code[104:] = bytes.fromhex('7047')
        data = configuration()
        data['abi'] = 2
        data['audio'] = dict(id='synthetic', ap_sha256=digest,
            config_sha256='11' * 32, flash_base=0x0c000000, ram_base=base,
            mmio_base=0x5fe00000, legacy_abi2=False,
            hooks=[dict(address=0x0c000000+i*8, entry=slot,
                        expected=list(guest[i*8:i*8+8])) for i, slot in enumerate(slots)])
        data['machine_properties'] = {'audio-mode': 'firmware',
            'audio-source': 'browser', 'audio-shim-hex': code.hex()}
        return data, guest

    def run_fixture(self, data, guest):
        # QMP uses stdio, so test the equivalent public file-config decoder.
        # Only synthetic inputs are written; product uses the startup pipe.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            ap = root/'guest.bin'; ap.write_bytes(guest)
            config = root/'hardware.json'; config.write_text(json.dumps(data))
            cmds = [dict(execute='qmp_capabilities'),
                dict(execute='qom-get', arguments=dict(path='/machine', property='audio-stats')),
                dict(execute='qom-set', arguments=dict(path='/machine', property='audio-shim-hex', value='00')),
                dict(execute='quit')]
            return subprocess.run([os.environ['MIBAND_QEMU'], '-M',
                f'bes1503,hardware-config={config}', '-kernel', str(ap), '-S',
                '-display', 'none', '-serial', 'null', '-monitor', 'none', '-qmp', 'stdio'],
                input='\n'.join(map(json.dumps, cmds))+'\n', text=True,
                capture_output=True, timeout=10)

    def test_inline_install_and_startup_lock(self):
        data, guest = self.fixture()
        result = self.run_fixture(data, guest)
        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
        stats = next(json.loads(row['return']) for row in records if isinstance(row.get('return'), str))
        self.assertTrue(stats['browser']['enabled'])
        self.assertFalse(stats['active'])
        self.assertIn('startup-only', result.stdout)

    def test_rejects_malformed_mismatched_or_ambiguous_helper(self):
        original, guest = self.fixture()
        code = original['machine_properties']['audio-shim-hex']
        cases = [dict(**{'audio-shim-hex': value}) for value in
                 ('', '0', 'gg'*106, '00'*65537, '00'*106,
                  code[:80]+'00'*32+code[144:])]
        cases.append({'audio-shim': 'not-a-file'})
        for mutation in cases:
            with self.subTest(mutation=str(mutation)[:80]):
                data = copy.deepcopy(original)
                data['machine_properties'].update(mutation)
                result = self.run_fixture(data, guest)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('audio adapter:', result.stderr)

    def test_audio_preserves_combined_binding_patches(self):
        data, guest = self.fixture()
        for section, offset in (('binding_auth', 56), ('binding_ui', 58)):
            data[section] = dict(id='synthetic', ap_sha256=hashlib.sha256(guest).hexdigest(),
                config_sha256='22'*32, flash_base=0x0c000000, patch_count=1,
                patches=[dict(address=0x0c000000+offset, guard_address=0x0c000000+offset,
                    patch_size=2, guard_size=2, expected=[0, 0xbf]+[0]*6,
                    replacement=[1, 0x20]+[0]*6, guard=[0, 0xbf]+[0]*30)])
        data['machine_properties'].update({'diag-skip-binding': True, 'diag-bypass-binding-auth': True})
        result = self.run_fixture(data, guest)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('binding auth adapter: synthetic applied', result.stderr)
        self.assertIn('binding UI adapter: synthetic site=', result.stderr)


if __name__ == '__main__':
    unittest.main()
