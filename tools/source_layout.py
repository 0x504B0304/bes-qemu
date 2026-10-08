"""Source manifest shared by QEMU integration and native regression builds."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def sources(root=ROOT):
    root = Path(root).resolve()
    manifest = json.loads((root / 'qemu/machine/sources.json').read_text())
    if set(manifest) != {'schema', 'sources'} or manifest['schema'] != 'bes.sources.v1':
        raise ValueError('unsupported source manifest')
    result = {}
    for relative in manifest['sources']:
        path = (root / relative).resolve(strict=True)
        if not path.is_relative_to(root) or path.suffix not in ('.c', '.h'):
            raise ValueError(f'invalid source path: {relative}')
        if path.name in result:
            raise ValueError(f'duplicate QEMU integration filename: {path.name}')
        result[path.name] = path
    return result


def source_file(name):
    return sources()[name]


def include_flags():
    return ['-I'+str(directory) for directory in sorted({p.parent for p in sources().values()})]
