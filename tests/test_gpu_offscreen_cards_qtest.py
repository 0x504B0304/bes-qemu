#!/usr/bin/env python3
"""reference captured card operations: BGRA5551 offscreen and rectangle fades."""
import argparse
import select
import struct
import subprocess

from test_gpu_bgr24_texture_qtest import fp, path_rectangle, state

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('qemu')
p.add_argument('ap_image')
a = p.parse_args()
q = subprocess.Popen([a.qemu, '-M',
    'miband-best1502p,display-width=432,display-height=514,display-te-pin=65',
    '-kernel', a.ap_image, '-display', 'none', '-monitor', 'none',
    '-serial', 'none', '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio'],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)


def cmd(s):
    q.stdin.write(s + '\n'); q.stdin.flush()
    assert select.select([q.stdout], [], [], 10)[0], 'QTest timeout'
    r = q.stdout.readline().strip()
    assert r.startswith('OK'), (s, r)
    return r[2:].strip()


def write(address, data):
    cmd(f'write {address:#x} {len(data)} 0x{data.hex()}')


def read(address, size):
    return bytes.fromhex(cmd(f'read {address:#x} {size}').removeprefix('0x'))


target, source, commands = 0x20001000, 0x3c200000, 0x38100000
regs = {
    0xa10: 0x105, 0xa11: target, 0xa12: 8, 0xa13: 3 | (2 << 16),
    0xa00: 0x10000102, 0xa02: 0xff0080ff, 0xad1: 0,
    # Poison stale texture state: a solid command must not consult it.
    0xa25: 0xffffffff, 0xa29: 0xffffffff, 0xa2f: 0,
    0xa34: 0x03300013, 0xa3b: fp(1), 0xa3c: 0, 0xa3d: 0x3a9,
    0xa40: fp(1), 0xa41: 0, 0xa42: 0,
    0xa43: 0, 0xa44: fp(1), 0xa45: 0,
    0xa39: 0, 0xa3a: 3 | (2 << 16), 0xacd: 0, 0xace: 0,
}


def submit(path=False):
    data = b''.join(state(k, v) for k, v in regs.items())
    data += (path_rectangle(0, 0, 3, 2) if path else
             struct.pack('<II4H', 0x40000001, 0, 0, 0, 3, 2))
    data += bytes(8)
    write(commands, data)
    cmd('writel 0x40220014 1')
    cmd(f'writel 0x40220500 {commands:#x}')
    cmd(f'writel 0x40220504 {len(data)//8}')
    return int(cmd('readl 0x40220010'), 0)


try:
    for path in (False, True):
        write(target-4, b'\x9d'*24)
        assert submit(path) == 1
        # A=1, R=0, G=16, B=31; keep two padding bytes per row.
        assert read(target-4, 24) == b'\x9d'*4 + (b'\x1f\x82'*3+b'\x9d'*2)*2 + b'\x9d'*4
    print('PASS BGRA5551 solid rectangle/path, stale-source isolation, pitch/guards')
    regs.update({0xa10: 0x103, 0xa12: 12, 0xa00: 0x10001181,
        0xa25: 0x01000104, 0xa27: 0, 0xa29: source, 0xa2b: 8,
        0xa2f: 3 | (2 << 16), 0xa18: fp(.5), 0xa19: fp(.5),
        0xa1a: fp(1), 0xa1c: fp(1), 0xa1d: 0, 0xa1e: 0,
        0xa20: 0, 0xa21: fp(1), 0xa22: 0})
    write(source, struct.pack('<8H', 0x801f, 0x83e0, 0xfc00, 0xdead,
                             0xffff, 0x8000, 0, 0xdead))
    write(target, bytes(24))
    assert submit() == 1
    assert read(target, 24) == bytes.fromhex('ff0000ff00ff00ff0000ffffffffffff000000ff00000000')
    print('PASS BGRA5551 texture color order/alpha, transparent source and pitch')
    # Captured transition mode10002181 with packed BGR24 source. Real alpha
    # modulation is tested against nonzero destination, not just completion.
    regs.update({0xa10: 0x109, 0xa12: 12, 0xa00: 0x10002181,
                 0xa02: 0xb2b2b2b2, 0xa25: 0x21020100, 0xa2b: 12,
                 0xa18: 0, 0xa19: 0})
    write(source, bytes([100, 80, 60])*6)
    write(target, bytes([20, 30, 40])*6)
    assert submit() == 1
    expected = bytes(((c*178+127)//255*255+d*77+127)//255
                     for c, d in zip([100, 80, 60], [20, 30, 40]))
    assert read(target, 18) == expected*6
    print('PASS BGR24 rectangle MULTIPLY fade, destination blend')
    regs[0xad1] = 1
    before = read(target, 18)
    assert submit() == 0 and read(target, 18) == before
    print('PASS unqualified global alpha still rejected without drawing/completion')
finally:
    q.terminate()
    try:
        q.wait(timeout=5)
    except subprocess.TimeoutExpired:
        q.kill(); q.wait()
