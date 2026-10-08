#!/usr/bin/env python3
"""Compile and run Xiaomi protobuf prepare request/response vectors."""
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
    include = str(source_file('miband_xiaomi_pb.h').parent)
    with tempfile.TemporaryDirectory(prefix='miband-xiaomi-pb-') as directory:
        directory = Path(directory)
        objects = []
        for name in ('miband_xiaomi_pb.c',):
            obj = directory / (name + '.o')
            subprocess.run(flags + ['-I' + str(ROOT), '-I' + include,
                                    '-c', str(source_file(name)), '-o', str(obj)],
                           cwd=build, check=True)
            objects.append(str(obj))
        test_obj = directory / 'test.o'
        subprocess.run(flags + ['-I' + str(ROOT), '-I' + include,
                                '-c', str(ROOT / 'test_xiaomi_pb.c'), '-o', str(test_obj)],
                       cwd=build, check=True)
        binary = directory / 'test_xiaomi_pb'
        subprocess.run(['clang-23', '-o', str(binary), *objects, str(test_obj)],
                       cwd=build, check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS Xiaomi protobuf prepare codec')


if __name__ == '__main__':
    main()
