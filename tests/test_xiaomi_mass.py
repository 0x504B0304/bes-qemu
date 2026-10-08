#!/usr/bin/env python3
"""Compile and run ordered Xiaomi mass-fragment reassembly vectors."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from tools.source_layout import source_file

ROOT = Path(__file__).resolve().parent


def main():
    build = Path(os.environ.get('QEMU_BUILD', '/tmp/miband-qemu-8.2/build'))
    commands = json.loads((build / 'compile_commands.json').read_text())
    command = next(item['command'] for item in commands
                   if item['file'].endswith('miband_hci_synthetic.c'))
    flags = shlex.split(command)[:shlex.split(command).index('-MD')]
    xiaomi_include = '-I' + str(source_file('miband_xiaomi_mass.h').parent)
    with tempfile.TemporaryDirectory(prefix='miband-xiaomi-mass-') as directory:
        directory = Path(directory)
        objects = []
        for name in ('miband_xiaomi_protocol.c', 'miband_xiaomi_mass.c'):
            obj = directory / (name + '.o')
            subprocess.run(flags + ['-I' + str(ROOT), xiaomi_include,
                                    '-c', str(source_file(name)), '-o', str(obj)],
                           cwd=build, check=True)
            objects.append(str(obj))
        test_obj = directory / 'test.o'
        subprocess.run(flags + ['-I' + str(ROOT), xiaomi_include,
                                '-c', str(ROOT / 'test_xiaomi_mass.c'),
                                '-o', str(test_obj)], cwd=build, check=True)
        binary = directory / 'test_xiaomi_mass'
        subprocess.run(['clang-23', '-o', str(binary), *objects, str(test_obj)],
                       cwd=build, check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS Xiaomi mass ordered fragment reassembly and CRC validation')


if __name__ == '__main__':
    main()
