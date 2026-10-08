#!/usr/bin/env python3
"""Pixel oracles for BEST1502P texel and BEST1503 normalized texture matrices."""
import argparse
import select
import struct
import subprocess

from test_gpu_bgr24_texture_qtest import fp, path_rectangle, state


def check(qemu, ap, texels):
    machine = ('miband-best1502p,display-width=432,display-height=514,display-te-pin=65'
               if texels else 'bes1503,gpu-render-index8=on,gpu-ack-no-render=off')
    proc = subprocess.Popen([qemu, '-M', machine, '-kernel', ap,
        '-display', 'none', '-monitor', 'none', '-serial', 'none', '-serial', 'none',
        '-accel', 'qtest', '-qtest', 'stdio'], stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

    def command(text):
        proc.stdin.write(text + '\n')
        proc.stdin.flush()
        assert select.select([proc.stdout], [], [], 10)[0], 'QTest timeout'
        result = proc.stdout.readline().strip()
        assert result.startswith('OK'), (text, result)
        return result[2:].strip()

    def write(address, data):
        command(f'write {address:#x} {len(data)} 0x{data.hex()}')

    target, source, commands = 0x20001000, 0x3c200000, 0x38100000
    colors = [0xff123456, 0xffabcdef, 0xffdd8844, 0xff336699, 0xff885522, 0xff9933cc]
    background = b'\x11\x22\x33'
    try:
        write(source, bytes([0, 1, 2, 99, 3, 4, 5, 99]))
        for path in (False, True):
            for rotated in (False, True):
                # Translate into a larger target; quarter-turn variant also
                # exchanges x/y contributions. Oracle uses discrete indices.
                if rotated:
                    matrix = [0.5, 3.5, 0, -1, 1, 0]
                    oracle = lambda x, y: (y, 3-x)
                else:
                    matrix = [-0.5, -0.5, 1, 0, 0, 1]
                    oracle = lambda x, y: (x-1, y-1)
                states = {
                    0xa10: 0x109, 0xa11: target, 0xa12: 24, 0xa13: 6 | (5 << 16),
                    0xa00: 0x10001102 if path else 0x10001181, 0xa02: 0, 0xad1: 0,
                    0xa25: 0x01000900, 0xa27: 0, 0xa29: source, 0xa2b: 4,
                    0xa2f: 3 | (2 << 16), 0xa1a: fp(1), 0xa1e: 0, 0xa22: 0,
                    0xa34: 0x03300013, 0xa3b: fp(1), 0xa3c: 0, 0xa3d: 0x3a9,
                    0xa40: fp(1), 0xa41: 0, 0xa42: 0,
                    0xa43: 0, 0xa44: fp(1), 0xa45: 0,
                    0xa39: 0, 0xa3a: 6 | (5 << 16), 0xacd: 0, 0xace: 0,
                }
                for i, reg in enumerate([0xa18, 0xa19, 0xa1c, 0xa1d, 0xa20, 0xa21]):
                    states[reg] = fp(matrix[i] / (1 if texels else (3 if i % 2 == 0 else 2)))
                states.update({0xb00+i: color for i, color in enumerate(colors)})
                write(target-8, b'\x9d'*8 + background*30 + b'\x9d'*8)
                payload = b''.join(state(reg, value) for reg, value in states.items())
                payload += (path_rectangle(0, 0, 6, 5) if path else
                            struct.pack('<II4H', 0x40000001, 0, 0, 0, 6, 5))
                payload += bytes(8)
                write(commands, payload)
                command('writel 0x40220014 1')
                command(f'writel 0x40220500 {commands:#x}')
                command(f'writel 0x40220504 {len(payload)//8}')
                assert int(command('readl 0x40220010'), 0) == 1
                expected = bytearray()
                for y in range(5):
                    for x in range(6):
                        sx, sy = oracle(x, y)
                        expected += (struct.pack('<I', colors[sy*3+sx])[:3]
                                     if 0 <= sx < 3 and 0 <= sy < 2 else background)
                actual = bytes.fromhex(command(f'read {target-8:#x} 106').removeprefix('0x'))
                assert actual == b'\x9d'*8 + expected + b'\x9d'*8, (texels, path, rotated, actual.hex(), expected.hex())
                print(f'PASS texels={texels} path={path} rotated={rotated}', flush=True)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('qemu')
    p.add_argument('ap_image')
    args = p.parse_args()
    for texel_mode in (False, True):
        check(args.qemu, args.ap_image, texel_mode)
