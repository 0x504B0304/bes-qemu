"""Small upstream-source fixtures for integration conflict tests."""
import difflib
from hashlib import sha256
import json
from pathlib import Path


def backport_fixture(root, qemu, edits=None, *, package_name='emmc-9.2'):
    if edits is None:
        edits = {'hw/sd/sd.c': (b'/* SD */\n', b'/* SD/MMC */\n')}
    package = Path(root) / 'qemu/machine/patches' / package_name
    package.mkdir(parents=True, exist_ok=True)
    Path(qemu).mkdir(parents=True, exist_ok=True)
    (Path(qemu) / 'VERSION').write_bytes(b'8.2.10\n')
    patch = []
    files = []
    for name, (before, after) in edits.items():
        path = Path(qemu) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(before)
        patch.extend(difflib.unified_diff(
            before.decode().splitlines(keepends=True),
            after.decode().splitlines(keepends=True),
            fromfile='a/' + name, tofile='b/' + name))
        files.append({'path': name, 'before_sha256': sha256(before).hexdigest(),
                      'after_sha256': sha256(after).hexdigest()})
    payload = ''.join(patch).encode()
    (package / 'emmc.patch').write_bytes(payload)
    (package / 'manifest.json').write_text(json.dumps({
        'schema': 'bes.qemu-backport.v1', 'qemu_version': '8.2.10',
        'patch': 'emmc.patch', 'patch_sha256': sha256(payload).hexdigest(),
        'files': files}), encoding='utf-8')
    return package
