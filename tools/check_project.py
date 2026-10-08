#!/usr/bin/env python3
"""Run this repository's unittest cases; hardware execution is opt-in."""
import argparse
import ast
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hardware', action='store_true')
    parser.add_argument('--qemu', type=Path)
    parser.add_argument('--list', action='store_true')
    args = parser.parse_args()
    modules = []
    for path in sorted((ROOT / 'tests').glob('test_*.py')):
        tree = ast.parse(path.read_text(encoding='utf-8'))
        cases = any(isinstance(n, ast.ClassDef) and any(
            isinstance(b, ast.Attribute) and b.attr in ('TestCase', 'IsolatedAsyncioTestCase')
            for b in n.bases) for n in tree.body)
        if cases and (args.hardware or not path.stem.endswith('_qtest')):
            modules.append('tests.' + path.stem)
    if args.list:
        print('\n'.join(modules))
        return 0
    environment = dict(os.environ)
    if args.qemu:
        qemu = str(args.qemu.resolve(strict=True))
        environment.update(MIBAND_QEMU=qemu, MIBAND_BEST1700_QEMU=qemu)
    return subprocess.call([sys.executable, '-m', 'unittest', *modules],
                           cwd=ROOT, env=environment)


if __name__ == '__main__':
    raise SystemExit(main())
