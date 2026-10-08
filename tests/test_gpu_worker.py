#!/usr/bin/env python3
"""Compile real GPU modules with clang 23 and QEMU headers; run private replay.

QEMU_BUILD points to an already configured clang-23 QEMU 8.2 build. This does not
change that checkout/build or start a system emulator or access physical UART.
"""
import json
from tools.source_layout import source_file, include_flags
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

source = Path(__file__).resolve().parent
build = Path(os.environ.get('QEMU_BUILD',
    '/tmp/miband-qemu-display-merge-20260930/build'))
commands = json.loads((build / 'compile_commands.json').read_text())
command = next(item['command'] for item in commands
               if item['file'].endswith('miband_gpu.c'))
flags = shlex.split(command)
flags = flags[:flags.index('-MD')]
compiler = flags[0]
flags += ['-I' + str(source)] + include_flags()
with tempfile.TemporaryDirectory(prefix='miband-gpu-worker-test-') as directory:
    directory = Path(directory)
    objects = []
    for name in ['miband_gpu.c', 'miband_gpu_worker.c',
                 'miband_display_memory.c', 'test_gpu_worker.c']:
        obj = directory / (name + '.o')
        subprocess.run(flags + ['-c', str(source / name if name.startswith('test_') else source_file(name)), '-o', str(obj)],
                       cwd=build, check=True)
        objects.append(str(obj))
    binary = directory / 'test_gpu_worker'
    libs = shlex.split(subprocess.check_output(
        ['pkg-config', '--libs', 'glib-2.0', 'gthread-2.0'], text=True))
    subprocess.run([compiler, '-o', str(binary)] + objects +
                   ['-pthread', '-lm'] + libs,
                   cwd=build, check=True)
    dump = directory / 'private-dump-must-remain-empty'
    dump.mkdir()
    environment = dict(os.environ, MIBAND_AGGRESSIVE_DUMP_DIR=str(dump),
                       MIBAND_GPU_DRAW_DUMP_LIMIT='16')
    subprocess.run([str(binary)], check=True, timeout=30, env=environment)
    assert not list(dump.iterdir()), 'private replay performed diagnostic file I/O'
    print('PASS private replay suppresses draw/gradient diagnostic file I/O')
