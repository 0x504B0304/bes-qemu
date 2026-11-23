#!/usr/bin/env python3
"""Fetch a pinned Android/Bionic dependency sysroot (never desktop Linux libs).

One lock per Termux architecture (aarch64, arm, i686, x86_64), matching the
Android ABIs arm64-v8a, armeabi-v7a, x86 and x86_64.

The lock is an explicit input, not a moving repository lookup during builds.
Use --update-lock only when intentionally qualifying a new dependency set.
Termux packages retain their notices; see the lock's source repository for
corresponding build recipes and source provenance.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import subprocess
import urllib.request

ROOT = Path(__file__).resolve().parent
REPOSITORY = 'https://packages.termux.dev/apt/termux-main/'
ARCHITECTURES = ('aarch64', 'arm', 'i686', 'x86_64')
ROOTS = ('glib', 'libpixman', 'libslirp', 'gdb', 'python-cryptography',
         'python-psutil', 'capstone', 'termux-licenses')


def records(text):
    result = {}
    for block in text.split('\n\n'):
        item = dict(line.split(': ', 1) for line in block.splitlines()
                    if ': ' in line and not line.startswith(' '))
        if 'Package' in item:
            result[item['Package']] = item
    return result


def create_lock(index, architecture):
    pending, selected = list(ROOTS), {}
    while pending:
        name = pending.pop()
        if name in selected:
            continue
        item = index[name]
        selected[name] = dict(name=name, version=item['Version'], size=int(item['Size']),
                              sha256=item['SHA256'], url=REPOSITORY + item['Filename'])
        for dependency in item.get('Depends', '').split(','):
            choices = [re.split(r'\s|\(', value.strip())[0] for value in dependency.split('|')]
            if choices != ['']:
                pending.append(next(value for value in choices if value in index))
    return dict(schema='bes.android.dependencies.v1', architecture=architecture,
                recipe_repository='https://github.com/termux/termux-packages',
                packages=[selected[name] for name in sorted(selected)])


def fetch(item, cache):
    path = cache / (item['sha256'] + '.deb')
    if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == item['sha256']:
        return path
    temporary = path.with_suffix('.pending')
    with urllib.request.urlopen(item['url'], timeout=120) as response, temporary.open('wb') as output:
        import shutil
        shutil.copyfileobj(response, output)
    if temporary.stat().st_size != item['size'] or hashlib.sha256(temporary.read_bytes()).hexdigest() != item['sha256']:
        temporary.unlink(missing_ok=True)
        raise ValueError('Android dependency checksum mismatch: ' + item['name'])
    temporary.replace(path)
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arch', choices=ARCHITECTURES, default='aarch64')
    parser.add_argument('--lock', type=Path)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path, required=True)
    parser.add_argument('--update-lock', type=Path, metavar='PACKAGES_INDEX')
    args = parser.parse_args()
    args.lock = args.lock or ROOT / f'dependencies-{args.arch}.lock.json'
    if args.update_lock:
        lock = create_lock(records(args.update_lock.read_text()), args.arch)
        args.lock.write_text(json.dumps(lock, indent=2) + '\n')
        return
    lock = json.loads(args.lock.read_text())
    if lock['schema'] != 'bes.android.dependencies.v1' or lock['architecture'] != args.arch:
        raise ValueError('unsupported Android dependency lock')
    args.cache.mkdir(parents=True, exist_ok=True)
    args.sysroot.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        files = list(pool.map(lambda item: fetch(item, args.cache), lock['packages']))
    for item, path in zip(lock['packages'], files):
        subprocess.run(['dpkg-deb', '-x', str(path), str(args.sysroot)], check=True)
        print(item['name'], item['version'], flush=True)
    (args.sysroot / 'dependency-lock.json').write_text(json.dumps(lock, indent=2) + '\n')


if __name__ == '__main__':
    main()
