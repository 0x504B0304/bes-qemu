#!/usr/bin/env python3
"""Verify board-sized RGB565 scanout, padded rows and immutable worker input."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

WIDTH, HEIGHT = 212, 520
LCDC = 0x40200000
DONE = (1 << 21) | (1 << 27)


def check(args, worker, address):
    run = args.run / (f'{address:08x}-' + ('worker' if worker else 'sync'))
    run.mkdir(parents=True)
    with socket.socket() as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    with (run / 'stderr.log').open('w') as log:
        q = subprocess.Popen([args.qemu, '-M',
            'bes1503,display-width=212,display-height=520,'
            f'framebuffer-bpp=16,lcdc-worker={"on" if worker else "off"},gpu-scanout-bridge=off',
            '-kernel', args.ap, '-display', 'none', '-monitor', 'none',
            '-serial', 'none', '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio',
            '-qmp', f'tcp:127.0.0.1:{port},server=on,wait=off'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True,
            env=dict(os.environ, MIBAND_LCDC_TEST_WORKER_DELAY_US='40000'))

        def command(line):
            q.stdin.write(line + '\n')
            q.stdin.flush()
            reply = q.stdout.readline().strip()
            assert reply.startswith('OK'), (line[:80], reply)
            return reply.split()[-1]

        def write(offset, value):
            command(f'writel {LCDC + offset:#x} {value:#x}')

        try:
            for _ in range(100):
                try:
                    sock = socket.create_connection(('127.0.0.1', port), .2)
                    break
                except OSError:
                    assert q.poll() is None, (run / 'stderr.log').read_text()
                    time.sleep(.02)
            else:
                raise TimeoutError('QMP listener')
            sock.settimeout(5)
            with sock, sock.makefile('rwb') as stream:
                assert 'QMP' in json.loads(stream.readline())

                def qmp(name, arguments=None):
                    stream.write((json.dumps({'execute': name, 'arguments': arguments or {}}) + '\n').encode())
                    stream.flush()
                    while True:
                        reply = json.loads(stream.readline())
                        assert 'error' not in reply, reply
                        if 'return' in reply:
                            return reply['return']

                qmp('qmp_capabilities')
                # Literal RGB565 wire values: red f800, blue 001f. Padding
                # must be skipped, and row 519 must survive conversion.
                row = b'\x00\xf8' * (WIDTH // 2) + b'\x1f\x00' * (WIDTH // 2)
                stride = WIDTH * 2 + 8
                source = (row + b'\x55' * 8) * HEIGHT
                expected = (b'\xff\x00\x00' * (WIDTH // 2) +
                            b'\x00\x00\xff' * (WIDTH // 2)) * HEIGHT
                command(f'write {address:#x} {len(source)} 0x{source.hex()}')
                write(0x104, (HEIGHT << 16) | WIDTH)
                write(0xfc, stride)
                write(0x264, 1)
                write(0xf4, address)
                write(0x1c4, 0)
                write(0x224, 1)
                command('clock_step 20000000')
                if worker:
                    assert int(command(f'readl {LCDC + 0x1c4:#x}'), 16) & DONE == 0
                    command(f'memset {address:#x} {len(source)} 0')
                deadline = time.monotonic() + 3
                while int(command(f'readl {LCDC + 0x1c4:#x}'), 16) & DONE != DONE:
                    assert time.monotonic() < deadline, 'missing scanout completion'
                    time.sleep(.002)
                path = run / 'scanout.ppm'
                qmp('screendump', {'filename': str(path)})
                with path.open('rb') as image:
                    assert image.readline() == b'P6\n'
                    assert image.readline() == b'212 520\n'
                    assert image.readline() == b'255\n'
                    pixels = image.read()
                assert pixels == expected, (len(pixels), pixels[:12].hex())
                # An out-of-bank request must neither read beyond host memory
                # nor complete or overwrite the panel's previous frame.
                end = 0x20160000 if address in (0x20000000, 0x00200000) else 0x3e000000
                write(0x1c4, 0)
                write(0xf4, end - 16)
                write(0x224, 1)
                command('clock_step 20000000')
                time.sleep(.06)
                assert int(command(f'readl {LCDC + 0x1c4:#x}'), 16) & DONE == 0
                qmp('screendump', {'filename': str(path)})
                assert path.read_bytes().split(b'255\n', 1)[1] == expected
                print(f'BOARD_212X520_RGB565_{address:08X}_{"WORKER" if worker else "SYNC"}_PADDED_BOUNDS_OK', flush=True)
        finally:
            q.terminate()
            try:
                q.wait(timeout=5)
            except subprocess.TimeoutExpired:
                q.kill()
                q.wait()
            q.stdin.close()
            q.stdout.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('qemu')
    parser.add_argument('ap')
    parser.add_argument('run', type=Path)
    args = parser.parse_args()
    args.run.mkdir(parents=True, exist_ok=False)
    def timeout(*_):
        raise TimeoutError('display geometry regression stalled')
    signal.signal(signal.SIGALRM, timeout)
    signal.alarm(30)
    try:
        for address in (0x20000000, 0x00200000, 0x3c000000,
                        0x38000000, 0x1c000000, 0x18000000):
            check(args, False, address)
            check(args, True, address)
    finally:
        signal.alarm(0)


if __name__ == '__main__':
    main()
