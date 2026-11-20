#!/usr/bin/env python3
"""INDEX4 POINT rectangle: low-nibble-first texels, b00 CLUT, padded rows and
strict rejection of a source stride that cannot hold the row."""
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
    # 4x2 INDEX4 texels, two per byte, first texel in the low nibble.
    # Row 0 = 1,2,3,4; row 1 = 5,6,7,15. Rows are 4 bytes apart; the two
    # padding bytes per row are poisoned and must never be sampled.
    write(source, bytes([0x21, 0x43, 0x99, 0x99, 0x65, 0xf7, 0x99, 0x99]))
    clut = {i: 0xff000000 | (i * 0x10) << 16 | (0xff - i * 0x10) << 8 | i
            for i in range(16)}
    sentinel = bytes([17, 23, 31])
    write(target, sentinel * 8)
    states = {
        0xa10: 0x109, 0xa11: target, 0xa12: 16, 0xa13: 4 | (2 << 16),
        0xa00: 0x10001181, 0xa02: 0, 0xad1: 0,
        0xa25: 0x01000700, 0xa27: 0, 0xa29: source,
        0xa2b: 4, 0xa2f: 4 | (2 << 16),
        # Normalized POINT mapping onto texel centres: u = (x + .5) / 4.
        0xa18: fp(0.125), 0xa19: fp(0.25), 0xa1a: fp(1),
        0xa1c: fp(0.25), 0xa1d: 0, 0xa1e: 0,
        0xa20: 0, 0xa21: fp(0.5), 0xa22: 0,
    }
    states.update({0xb00 + i: c for i, c in clut.items()})

    def submit():
        data = b''.join(struct.pack('<II', 0x30010000 | reg, val)
                        for reg, val in states.items())
        data += struct.pack('<II4HII', 0x40000001, 0, 0, 0, 4, 2, 0, 0)
        write(commands, data)
        cmd('writel 0x40220014 1')
        cmd(f'writel 0x40220500 {commands:#x}')
        cmd(f'writel 0x40220504 {len(data) // 8}')
        return int(cmd('readl 0x40220010'), 0)

    assert submit() == 1, 'INDEX4 command must complete after rendering'
    actual = bytes.fromhex(cmd(f'read {target:#x} 24').removeprefix('0x'))
    # Opaque CLUT entries replace the destination; packed target is B,G,R.
    expected = bytearray()
    for index in [1, 2, 3, 4, 5, 6, 7, 15]:
        color = clut[index]
        expected += bytes([color & 255, (color >> 8) & 255, (color >> 16) & 255])
    assert actual == expected, (actual.hex(), expected.hex())

    # A 1-byte stride cannot hold four 4-bit texels: no draw, no completion.
    write(target, sentinel * 8)
    states[0xa2b] = 1
    assert submit() == 0
    assert bytes.fromhex(cmd(f'read {target:#x} 24').removeprefix('0x')) == sentinel * 8
    print('PASS INDEX4 POINT nibble order, CLUT b00, row stride, strict rejection')
finally:
    q.terminate()
    try:
        q.wait(timeout=5)
    except subprocess.TimeoutExpired:
        q.kill()
        q.wait()
