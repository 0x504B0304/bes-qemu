#!/usr/bin/env python3
"""Compile LCDC DMA memory resolution with real QEMU headers, then test it."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

from tools.source_layout import include_flags, source_file


def main():
    source = Path(__file__).resolve().parent
    build = Path(os.environ.get(
        'QEMU_BUILD', '/tmp/miband-qemu-aggressive-clang23/build'))
    commands = json.loads((build / 'compile_commands.json').read_text())
    command = next(item['command'] for item in commands
                   if item['file'].endswith('miband_lcdc.c'))
    flags = shlex.split(command)
    flags = flags[:flags.index('-MD')]
    flags += include_flags() + ['-ffunction-sections', '-fdata-sections']
    with tempfile.TemporaryDirectory(prefix='miband-lcdc-memory-') as directory:
        directory = Path(directory)
        objects = []
        for name in ('miband_lcdc.c', 'miband_display_memory.c',
                     'test_lcdc_memory.c'):
            path = source / name if name.startswith('test_') else source_file(name)
            obj = directory / (name + '.o')
            subprocess.run(flags + ['-c', str(path), '-o', str(obj)],
                           cwd=build, check=True)
            objects.append(str(obj))
        binary = directory / 'test'
        libs = shlex.split(subprocess.check_output(
            ['pkg-config', '--libs', 'glib-2.0'], text=True))
        subprocess.run([flags[0], '-Wl,--gc-sections', '-o', str(binary),
                        *objects, *libs], cwd=build, check=True)
        subprocess.run([str(binary)], timeout=10, check=True)


if __name__ == '__main__':
    main()
