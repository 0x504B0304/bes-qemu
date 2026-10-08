"""Validate a SoC package selection and produce immutable C build constants."""
import json
import hashlib
from pathlib import Path
import re


def fields(value, expected, label):
    if not isinstance(value, dict) or set(value) != set(expected):
        raise ValueError(f'{label}: expected fields {sorted(expected)}')


def load_package(soc_path):
    soc_path = Path(soc_path).resolve()
    soc = json.loads(soc_path.read_text())
    fields(soc, ['schema', 'id', 'package'], 'SoC')
    if soc['schema'] != 'bes.soc.v1' or soc['id'] != 'best1503':
        raise ValueError('unsupported SoC integration')
    path = (soc_path.parent / soc['package']).resolve()
    if not path.is_relative_to(soc_path.parent / 'package/variants'):
        raise ValueError('package must belong to the selected SoC')
    package = json.loads(path.read_text())
    fields(package, ['schema', 'id', 'soc', 'evidence', 'nor', 'psram'], 'package')
    if package['schema'] != 'bes.package.v1' or package['soc'] != soc['id']:
        raise ValueError('package/SoC mismatch')
    if not isinstance(package['id'], str) or not package['id']:
        raise ValueError('package identity required')
    fields(package['nor'], ['size', 'sector_size', 'jedec_id'], 'NOR')
    fields(package['psram'], ['size', 'mr1'], 'PSRAM')
    for label, limit in (('nor', 0x1000000), ('psram', 0x2000000)):
        size = package[label]['size']
        # This initial integration supports its existing alias geometry. New
        # package capacities require explicitly updating that SoC integration.
        if type(size) is not int or size != limit:
            raise ValueError(f'{label}: capacity unsupported by current BEST1503 integration')
    mr1 = package['psram']['mr1']
    if type(mr1) is not int or not 0 <= mr1 <= 0xffffffff:
        raise ValueError('PSRAM MR1 must be a uint32')
    sector = package['nor']['sector_size']
    if type(sector) is not int or not 256 <= sector <= 0x40000 or sector & (sector - 1):
        raise ValueError('NOR sector size must be a supported power of two')
    identity = package['nor']['jedec_id']
    if not isinstance(identity, list) or len(identity) != 3 or any(
            type(v) is not int or not 0 <= v <= 255 for v in identity):
        raise ValueError('NOR JEDEC ID must contain three bytes')
    return package


def load_best1700_package(path):
    """Validate the runtime NOR identity for the BEST1700 flash geometry."""
    package = json.loads(Path(path).read_text(encoding='utf-8'))
    expected = ['schema', 'id', 'soc', 'evidence', 'nor']
    if isinstance(package, dict) and 'bth_nor' in package:
        expected.append('bth_nor')
    fields(package, expected, 'BEST1700 package')
    if package['schema'] != 'bes.package.best1700.v1' or package['soc'] != 'best1700':
        raise ValueError('BEST1700 package/SoC mismatch')
    if (not isinstance(package['id'], str) or
            not re.fullmatch(r'[a-z0-9]+(?:-[a-z0-9]+)*', package['id'])):
        raise ValueError('BEST1700 package identity must be an identifier')
    if not isinstance(package['evidence'], str) or not package['evidence'].strip():
        raise ValueError('BEST1700 package requires evidence')
    for name, label in [('nor', 'NOR'), ('bth_nor', 'BTH NOR')]:
        if name in package:
            validate_best1700_nor(package[name], label)
    return package


def validate_best1700_nor(nor, label):
    """Validate one independently selected flash die set."""
    fields(nor, ['size', 'die_count', 'jedec_id', 'uid0', 'uid1'],
           'BEST1700 ' + label)
    if type(nor['size']) is not int or nor['size'] != 0x1000000:
        raise ValueError(f'{label} capacity unsupported by current BEST1700 integration')
    if type(nor['die_count']) is not int or nor['die_count'] not in (1, 2):
        raise ValueError(f'{label} die_count must be 1 or 2')
    identity = nor['jedec_id']
    if not isinstance(identity, list) or len(identity) != 3 or any(
            type(value) is not int or not 0 <= value <= 255 for value in identity):
        raise ValueError(f'{label} JEDEC ID must contain three bytes')
    if (1 << identity[2]) * nor['die_count'] != nor['size']:
        raise ValueError(f'{label} JEDEC capacity must match the die count and package size')
    for name in ('uid0', 'uid1'):
        if type(nor[name]) is not int or not 0 <= nor[name] <= 0xffffffffffffffff:
            raise ValueError(f'{label} {name} must be a uint64')


def best1700_nor_machine_properties(package):
    """Keep SYS and optional BTH identities independent in launch and QMP checks."""
    properties = {}
    for name, prefix in [('nor', 'nor'), ('bth_nor', 'bth-nor')]:
        if name not in package:
            continue
        nor = package[name]
        properties.update({
            f'{prefix}-die-count': nor['die_count'],
            f'{prefix}-jedec-id': int.from_bytes(bytes(nor['jedec_id']), 'big'),
            f'{prefix}-uid0': nor['uid0'],
            f'{prefix}-uid1': nor['uid1'],
        })
    return properties


def fingerprint(package):
    return hashlib.sha256(json.dumps(package, sort_keys=True,
        separators=(',', ':')).encode()).hexdigest()


def header(package):
    return ("/* Generated from the selected SoC package. */\n"
            "#ifndef BEST1503_PACKAGE_CONFIG_H\n#define BEST1503_PACKAGE_CONFIG_H\n"
            f"#define BEST1503_PACKAGE_NOR_SIZE {package['nor']['size']:#x}u\n"
            f"#define BEST1503_PACKAGE_NOR_SECTOR_SIZE {package['nor']['sector_size']:#x}u\n"
            '#define BEST1503_PACKAGE_NOR_JEDEC_ID {' +
            ', '.join(hex(v) for v in package['nor']['jedec_id']) + '}\n' +
            '#define BEST1503_PACKAGE_CONFIG_SHA256 ' + json.dumps(fingerprint(package)) + '\n' +
            f"#define BEST1503_PACKAGE_PSRAM_SIZE {package['psram']['size']:#x}u\n"
            f"#define BEST1503_PACKAGE_PSRAM_MR1 {package['psram']['mr1']:#x}u\n"
            "#endif\n").encode()
