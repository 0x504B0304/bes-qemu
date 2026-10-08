#!/usr/bin/env python3
"""A8 LINEAR glyph pixels, padded rows, COLOR border and strict completion."""
import argparse
import struct
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('qemu')
p.add_argument('ap_image')
a = p.parse_args()
q = subprocess.Popen([a.qemu, '-M',
    'bes1503,gpu-render-index8=on,gpu-ack-no-render=off',
    '-kernel', a.ap_image, '-display', 'none', '-monitor', 'none',
    '-accel', 'qtest', '-qtest', 'stdio', '-serial', 'none', '-serial', 'none'],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
    text=True)


def cmd(s):
    q.stdin.write(s + '\n')
    q.stdin.flush()
    r = q.stdout.readline().strip()
    assert r.startswith('OK'), (s, r)
    return r[2:].strip()


def write(addr, data):
    cmd(f'write {addr:#x} {len(data)} 0x{data.hex()}')


def fp(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]


try:
    target, source, commands = 0x20001000, 0x3c200000, 0x38100000
    # Two 2-pixel rows with different coverage; poison the padding.
    write(source, bytes([0, 255, 99, 99, 128, 64, 99, 99]))
    sentinel = bytes([17, 23, 31])
    write(target, sentinel * 8)
    states = {
        0xa10: 0x109, 0xa11: target, 0xa12: 16, 0xa13: 4 | (2 << 16),
        0xa00: 0x2181, 0xa02: 0xff804020, 0xad1: 0,
        0xa25: 0x01010102, 0xa27: 0, 0xa29: source,
        0xa2b: 4, 0xa2f: 2 | (2 << 16),
        0xa18: fp(-0.25), 0xa19: fp(0.25), 0xa1a: fp(1),
        0xa1c: fp(0.5), 0xa1d: 0, 0xa1e: 0,
        0xa20: 0, 0xa21: fp(0.5), 0xa22: 0,
    }

    def submit():
        data = b''.join(struct.pack('<II', 0x30010000 | reg, val)
                        for reg, val in states.items())
        data += struct.pack('<II4HII', 0x40000001, 0, 0, 0, 4, 2, 0, 0)
        write(commands, data)
        cmd('writel 0x40220014 1')
        cmd(f'writel 0x40220500 {commands:#x}')
        cmd(f'writel 0x40220504 {len(data) // 8}')
        return int(cmd('readl 0x40220010'), 0)

    assert submit() == 1, 'A8 command must complete after rendering'
    actual = bytes.fromhex(cmd(f'read {target:#x} 24').removeprefix('0x'))
    # Independent horizontal coverage oracle. Vertical coordinates select
    # distinct rows (no bilinear interpolation); outside taps are transparent.
    expected = bytearray()
    for coverage in [0, 128, 128, 0, 64, 96, 32, 0]:
        for color, background in zip([32, 64, 128], sentinel):
            premult = (color * coverage + 127) // 255
            expected.append((premult * 255 + background * (255-coverage) + 127) // 255)
    assert actual == expected, (actual.hex(), expected.hex())
    # Unknown image mode must neither draw nor fabricate completion.
    states[0xa00] = 0x6181
    assert submit() == 0
    assert bytes.fromhex(cmd(f'read {target:#x} 24').removeprefix('0x')) == actual
    print('PASS A8 LINEAR coverage, row stride, COLOR border, MULTIPLY, strict completion')
finally:
    q.terminate()
    try:
        q.wait(timeout=5)
    except subprocess.TimeoutExpired:
        q.kill()
        q.wait()
