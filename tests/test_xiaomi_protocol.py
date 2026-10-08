#!/usr/bin/env python3
"""Compile and run the standalone Xiaomi L1/L2/mass codec vectors."""
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
    flags = shlex.split(command)
    flags = flags[:flags.index('-MD')]
    xiaomi_include = '-I' + str(source_file('miband_xiaomi_protocol.h').parent)
    with tempfile.TemporaryDirectory(prefix='miband-xiaomi-protocol-') as directory:
        binary = Path(directory) / 'test_xiaomi_protocol'
        subprocess.run(flags + ['-I' + str(ROOT), xiaomi_include,
                                '-c', str(source_file('miband_xiaomi_protocol.c')),
                                '-o', str(Path(directory) / 'protocol.o')], cwd=build, check=True)
        subprocess.run(flags + ['-I' + str(ROOT), xiaomi_include,
                                '-c', str(ROOT / 'test_xiaomi_protocol.c'),
                                '-o', str(Path(directory) / 'test.o')], cwd=build, check=True)
        subprocess.run(['clang-23', '-o', str(binary), str(Path(directory) / 'protocol.o'),
                        str(Path(directory) / 'test.o')], cwd=build, check=True)
        subprocess.run([str(binary)], check=True)
    print('PASS Xiaomi L1/L2/mass CRC and round-trip vectors')


if __name__ == '__main__':
    main()
