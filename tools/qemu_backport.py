"""Strict, offline installation of pinned QEMU source integrations."""
from hashlib import sha256
import json
from pathlib import Path, PurePosixPath
import re

from tools.source_layout import ROOT


def _inside(directory, name):
    relative = PurePosixPath(name)
    if relative.is_absolute() or not relative.parts or '..' in relative.parts or '\\' in name:
        raise ValueError(f'unsafe backport path: {name}')
    path = Path(directory).joinpath(*relative.parts)
    if not path.resolve().is_relative_to(Path(directory).resolve()):
        raise ValueError(f'backport path escapes directory: {name}')
    return path


def _sections(patch):
    """Read our unified diff format; never permit fuzzy matching or file creation."""
    result = {}
    lines = patch.splitlines(keepends=True)
    i = 0
    while i < len(lines):
        if not lines[i].startswith(b'--- a/'):
            raise ValueError('invalid backport file header')
        name = lines[i][6:].rstrip(b'\n').decode('utf-8')
        i += 1
        if i >= len(lines) or lines[i] != b'+++ b/' + name.encode() + b'\n':
            raise ValueError(f'mismatched backport file header: {name}')
        i += 1
        start = i
        while i < len(lines) and not lines[i].startswith(b'--- a/'):
            i += 1
        if name in result:
            raise ValueError(f'duplicate backport file: {name}')
        result[name] = lines[start:i]
    return result


def _apply(original, diff, name):
    source = original.splitlines(keepends=True)
    result = []
    consumed = 0
    i = 0
    while i < len(diff):
        match = re.fullmatch(rb'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@\n', diff[i])
        if not match:
            raise ValueError(f'invalid backport hunk: {name}')
        old_line, old_count, new_line, new_count = (
            int(value) if value is not None else 1 for value in match.groups())
        offset = old_line - 1 if old_count else old_line
        new_offset = new_line - 1 if new_count else new_line
        if not consumed <= offset <= len(source):
            raise ValueError(f'invalid backport offset: {name}')
        result.extend(source[consumed:offset])
        if len(result) != new_offset:
            raise ValueError(f'invalid backport output offset: {name}')
        consumed = offset
        i += 1
        old_used = new_used = 0
        while i < len(diff) and not diff[i].startswith(b'@@ '):
            line = diff[i]
            kind, content = line[:1], line[1:]
            if kind not in (b' ', b'-', b'+'):
                raise ValueError(f'invalid backport hunk line: {name}')
            if kind in (b' ', b'-'):
                if consumed >= len(source) or source[consumed] != content:
                    raise ValueError(f'backport context mismatch: {name}')
                consumed += 1
                old_used += 1
            if kind in (b' ', b'+'):
                result.append(content)
                new_used += 1
            i += 1
        if (old_used, new_used) != (old_count, new_count):
            raise ValueError(f'invalid backport hunk length: {name}')
    result.extend(source[consumed:])
    return b''.join(result)


def prepare_backport(qemu, root=ROOT, *, package_name, label='QEMU', prepared_files=None, _reverse_known=False):
    """Validate every input before returning writes; applied files are no-ops.

    Only exact upstream or exact previously patched bytes are accepted. A
    partially installed package can be completed, but unknown local edits fail
    before either upstream files or emulator-owned files are changed.
    """
    qemu = Path(qemu)
    package = _inside(Path(root) / 'qemu/machine/patches', package_name)
    manifest = json.loads((package / 'manifest.json').read_text(encoding='utf-8'))
    if manifest.get('schema') != 'bes.qemu-backport.v1':
        raise ValueError('unsupported QEMU backport manifest')
    version = (qemu / 'VERSION').read_text(encoding='ascii').strip()
    if version != manifest['qemu_version']:
        raise ValueError(f'{label} backport requires QEMU {manifest["qemu_version"]}, got {version}')
    payload = _inside(package, manifest['patch']).read_bytes()
    if sha256(payload).hexdigest() != manifest['patch_sha256']:
        raise ValueError(f'{label} backport patch checksum mismatch')
    sections = _sections(payload)
    names = [item['path'] for item in manifest['files']]
    if not names or len(names) != len(set(names)) or set(names) != set(sections):
        raise ValueError(f'{label} backport file list mismatch')
    updates = []
    for item in manifest['files']:
        name = item['path']
        path = _inside(qemu, name)
        original = (prepared_files[path] if prepared_files is not None and path in prepared_files
                    else path.read_bytes())
        digest = sha256(original).hexdigest()
        if _reverse_known:
            if digest == item['after_sha256']:
                updated = _apply(original, _reverse(sections[name]), name)
                if sha256(updated).hexdigest() != item['before_sha256']:
                    raise ValueError(f'{label} reverse checksum mismatch: {name}')
                updates.append((path, updated))
            # A later package may not have been applied yet. The forward
            # pass validates every intermediate hash before any file is written.
            continue
        if digest == item['after_sha256']:
            continue
        if digest != item['before_sha256']:
            raise ValueError(f'{label} backport conflict: {name}; unknown local content ({digest})')
        updated = _apply(original, sections[name], name)
        if sha256(updated).hexdigest() != item['after_sha256']:
            raise ValueError(f'{label} backport result checksum mismatch: {name}')
        updates.append((path, updated))
    return updates


def _reverse(diff):
    result = []
    for line in diff:
        if line.startswith(b'@@ '):
            match = re.fullmatch(rb'@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@\n', line)
            if not match:
                raise ValueError('invalid reverse backport hunk')
            a, b, c, d = (int(v) if v is not None else 1 for v in match.groups())
            result.append(f'@@ -{c},{d} +{a},{b} @@\n'.encode())
        elif line.startswith(b'+'):
            result.append(b'-' + line[1:])
        elif line.startswith(b'-'):
            result.append(b'+' + line[1:])
        else:
            result.append(line)
    return result


def prepare_backport_stack(qemu, root=ROOT, *, packages):
    """Validate ordered packages sharing files, including partial installations.

    Undo only exact known results in memory, then reapply the complete stack.
    Unknown source edits fail before any disk mutation. Return final writes
    only, so repeating an installation is an exact no-op.
    """
    prepared = {}
    for name, label in reversed(packages):
        prepared.update(prepare_backport(qemu, root, package_name=name,
            label=label, prepared_files=prepared, _reverse_known=True))
    for name, label in packages:
        prepared.update(prepare_backport(qemu, root, package_name=name,
            label=label, prepared_files=prepared))
    return [(path, content) for path, content in prepared.items()
            if path.read_bytes() != content]
