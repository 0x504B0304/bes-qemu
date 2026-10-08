#!/usr/bin/env python3
"""reference native DSI command sequence and board-owned DCS response state."""
import argparse
import select
import subprocess

p = argparse.ArgumentParser()
p.add_argument('qemu')
p.add_argument('ap_image')
a = p.parse_args()
q = subprocess.Popen([a.qemu, '-M',
    'miband-best1502p,display-width=432,display-height=514,panel-id=0x450120',
    '-kernel', a.ap_image, '-display', 'none', '-monitor', 'none',
    '-serial', 'none', '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio'],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)


def cmd(line):
    q.stdin.write(line + '\n'); q.stdin.flush()
    assert select.select([q.stdout], [], [], 5)[0], 'QTest timeout'
    reply = q.stdout.readline().strip()
    assert reply.startswith('OK'), (line, reply)
    return int(reply.split()[1], 0) if len(reply.split()) > 1 else None


def w(offset, value):
    cmd(f'writel {0x40230000 + offset:#x} {value:#x}')


def r(offset):
    return cmd(f'readl {0x40230000 + offset:#x}')


def request(command, length):
    w(0x10, 0x00ff8000)
    w(0x18, length << 8 | 0x37)
    w(0x14, 0x5e104)
    w(0x18, command << 8 | 6)
    w(0x14, 0x5e106)


def read(command, length):
    request(command, length)
    result = []
    while True:
        value = r(0x10)
        assert value & 0x00440000 == 0, f'RX error {value:x}'
        if (value >> 8) & 31:
            result.append(value & 255)
        else:
            assert value & 0x80000, 'RX completion absent'
            break
        assert len(result) <= length + 7
    if length == 1:
        assert len(result) == 5 and result[:2] == [0x87, 0x21], result
        return result[2:3]
    assert len(result) == length + 7 and result[:4] == [0x87, 0x1c, length, 0], result
    return result[5:5 + length]


def write(command, parameter=None):
    w(0x18, command << 8 | (5 if parameter is None else (parameter << 16 | 0x15)))
    w(0x14, 0x5e104)
    assert r(0x14) & 0x10 == 0


try:
    assert read(4, 3) == [0x45, 1, 0x20]
    assert read(0xda, 1) == [0x45]
    write(0x01)
    assert read(0x0a, 1) == [8]
    write(0x11); write(0x29)
    assert read(0x0a, 1) == [0x1c]
    write(0x35, 0)
    assert read(0x0e, 1) == [0x80]
    write(0x51, 0xff)
    assert read(0x52, 1) == [0xff]
    write(0x28); write(0x10); write(0x34)
    assert read(0x0a, 1) == [8] and read(0x0e, 1) == [0]
    request(0xff, 1)
    status = r(0x10)
    assert status & 0x1fff == 0 and status >> 16 & 0x44 == 0x44
    # AON resets the controller's RX FIFO/latches, not an unrelated RAM patch.
    request(4, 3)
    cmd('writel 0x400800c8 0x80')
    for _ in range(30):
        if cmd('readl 0x400800c8') == 0:
            break
    else:
        raise AssertionError('AP reset did not complete')
    assert r(0x10) == 0
    assert read(4, 3) == [0x45, 1, 0x20]
    print('PASS reference panel DSI: identity FIFO, DCS state, failure and AP reset')
finally:
    q.terminate(); q.wait(timeout=5)
    q.stdin.close(); q.stdout.close()
