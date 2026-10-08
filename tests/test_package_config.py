import json
import copy
from pathlib import Path
import tempfile
import unittest
from tools.source_layout import ROOT
from tools.package_config import header, load_package, load_best1700_package


class PackageConfig(unittest.TestCase):
    def test_current_package_emits_capacity_and_identity(self):
        package = load_package(ROOT/'qemu/soc/best1503/config.json')
        result = header(package)
        self.assertIn(b'0x2000000u', result)
        self.assertIn(b'PSRAM_MR1 0xdu', result)

    def test_invalid_package_rejected(self):
        original = load_package(ROOT/'qemu/soc/best1503/config.json')
        for field, value in [('soc', 'best1700'), ('schema', 'bes.package.v9'),
                             ('unknown', True), ('psram', {'size': 1, 'mr1': 13}),
                             ('psram', {'size': 33554432, 'mr1': True})]:
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root/'package/variants').mkdir(parents=True)
                (root/'config.json').write_text(json.dumps({'schema': 'bes.soc.v1',
                    'id': 'best1503', 'package': 'package/variants/test.json'}))
                package = {**original, field: value}
                (root/'package/variants/test.json').write_text(json.dumps(package))
                with self.assertRaises(ValueError):
                    load_package(root/'config.json')

    def test_nor_identity_and_sector_validation(self):
        original = load_package(ROOT/'qemu/soc/best1503/config.json')
        for field, value in [('sector_size', True), ('sector_size', 3000),
                             ('sector_size', 0), ('jedec_id', [200, 96]),
                             ('jedec_id', [200, False, 23])]:
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root/'package/variants').mkdir(parents=True)
                (root/'config.json').write_text(json.dumps({'schema': 'bes.soc.v1',
                    'id': 'best1503', 'package': 'package/variants/test.json'}))
                package = json.loads(json.dumps(original))
                package['nor'][field] = value
                (root/'package/variants/test.json').write_text(json.dumps(package))
                with self.assertRaises(ValueError):
                    load_package(root/'config.json')


class Best1700PackageConfig(unittest.TestCase):
    variant = ROOT / 'qemu/soc/best1700/package/variants/bes2800bp-compat.json'

    def test_compatibility_package_has_two_matching_dies_and_synthetic_uids(self):
        package = load_best1700_package(self.variant)
        self.assertEqual(package['id'], 'bes2800bp-compat')
        self.assertEqual(package['nor']['size'], 16 * 1024 * 1024)
        self.assertEqual(package['nor']['die_count'], 2)
        self.assertEqual(package['nor']['jedec_id'], [0xC8, 0x60, 0x17])
        self.assertNotEqual(package['nor']['uid0'], package['nor']['uid1'])

    def test_invalid_package_and_nor_fields_are_rejected(self):
        original = load_best1700_package(self.variant)
        package_cases = [('schema', 'bes.package.v1'), ('soc', 'best1503'),
                         ('id', '../package'), ('id', None), ('id', True),
                         ('evidence', ''), ('evidence', None)]
        nor_cases = [('size', 8 * 1024 * 1024), ('size', True),
                     ('size', 16777216.0), ('size', '16777216'),
                     ('die_count', 0), ('die_count', 3), ('die_count', True),
                     ('die_count', 2.0), ('die_count', '2'),
                     ('jedec_id', [0xC8, 0x60]), ('jedec_id', [0xC8, False, 0x17]),
                     ('jedec_id', [0xC8, 0x60, 0x18]),
                     ('jedec_id', [0xC8, 0x60, -1]), ('jedec_id', 'c86017')]
        for key in ('uid0', 'uid1'):
            nor_cases += [(key, value) for value in (-1, 1 << 64, True, 1.0, '1')]
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'package.json'
            bth_cases = [(key, [0xC8, 0x60, 0x17]
                          if key == 'jedec_id' and value == [0xC8, 0x60, 0x18]
                          else value) for key, value in nor_cases]
            for group, cases in ((None, package_cases), ('nor', nor_cases),
                                 ('bth_nor', bth_cases)):
                for key, value in cases:
                    with self.subTest(group=group, key=key, value=value):
                        package = copy.deepcopy(original)
                        target = package[group] if group else package
                        target[key] = value
                        path.write_text(json.dumps(package), encoding='utf-8')
                        with self.assertRaises(ValueError):
                            load_best1700_package(path)

    def test_bth_nor_is_an_independent_compatibility_selection(self):
        package = load_best1700_package(self.variant)
        self.assertEqual(package['nor']['die_count'], 2)
        self.assertEqual(package['nor']['jedec_id'], [0xC8, 0x60, 0x17])
        self.assertEqual(package['bth_nor']['size'], 16 * 1024 * 1024)
        self.assertEqual(package['bth_nor']['die_count'], 1)
        self.assertEqual(package['bth_nor']['jedec_id'], [0xC8, 0x60, 0x18])
        self.assertEqual(len({package[group][uid]
                              for group in ('nor', 'bth_nor')
                              for uid in ('uid0', 'uid1')}), 4)

    def test_legacy_package_does_not_inherit_or_invent_bth_nor(self):
        package = load_best1700_package(self.variant)
        del package['bth_nor']
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'package.json'
            path.write_text(json.dumps(package), encoding='utf-8')
            self.assertEqual(load_best1700_package(path), package)

    def test_bth_nor_must_have_an_exact_schema(self):
        original = load_best1700_package(self.variant)
        cases = [None, False, [], 'c86018', {},
                 dict(original['bth_nor'], unexpected=1)]
        cases.extend({key: value for key, value in original['bth_nor'].items()
                      if key != missing} for missing in original['bth_nor'])
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'package.json'
            for value in cases:
                with self.subTest(bth_nor=value):
                    package = dict(original, bth_nor=value)
                    path.write_text(json.dumps(package), encoding='utf-8')
                    with self.assertRaisesRegex(ValueError, 'BEST1700 BTH NOR'):
                        load_best1700_package(path)

    def test_single_die_uses_matching_jedec_capacity(self):
        package = load_best1700_package(self.variant)
        package['nor'].update(die_count=1, jedec_id=[0xC8, 0x60, 0x18],
                              uid0=0, uid1=0xffffffffffffffff)
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'package.json'
            path.write_text(json.dumps(package), encoding='utf-8')
            self.assertEqual(load_best1700_package(path), package)


if __name__ == '__main__':
    unittest.main()
