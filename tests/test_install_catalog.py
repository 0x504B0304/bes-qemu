#!/usr/bin/env python3
"""Verify completed Xiaomi mass blobs survive a flash journal restart."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from tools.source_layout import include_flags, source_file

ROOT = Path(__file__).resolve().parent

build = Path(os.environ.get('QEMU_BUILD', '/tmp/miband-qemu-8.2/build'))
commands = json.loads((build / 'compile_commands.json').read_text())
command = next(item['command'] for item in commands
               if item['file'].endswith('miband_hci_synthetic.c'))
flags = shlex.split(command)[:shlex.split(command).index('-MD')]
pkg_flags = shlex.split(subprocess.check_output(
    ['pkg-config', '--cflags', '--libs', 'glib-2.0', 'zlib'], text=True))

with tempfile.TemporaryDirectory(prefix='miband-install-catalog-') as directory:
    directory = Path(directory)
    binary = directory / 'test'
    subprocess.run([
        *flags,
        str(ROOT / 'test_install_catalog.c'),
        str(source_file('miband_install_catalog.c')),
        str(source_file('miband_store.c')),
        *include_flags(), *pkg_flags, '-o', str(binary)], cwd=build, check=True)
    subprocess.run([str(binary), str(directory / 'flash.journal')], check=True)
print('PASS install catalog persistence and journal replay')
