#!/usr/bin/env python3
"""ISPI GPADC polling: conversion time, masks, W1C, interval and reset."""
import argparse
import select
import subprocess

p = argparse.ArgumentParser()
p.add_argument('qemu')
p.add_argument('ap_image')
a = p.parse_args()
q = subprocess.Popen([a.qemu, '-M', 'miband-best1502p,gpadc-raw2=17908,gpadc-raw3=20000',
    '-kernel', a.ap_image, '-display', 'none', '-monitor', 'none',
    '-serial', 'none', '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio'],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)


def cmd(line):
    q.stdin.write(line + '\n'); q.stdin.flush()
    assert select.select([q.stdout], [], [], 5)[0], 'QTest timeout'
    reply = q.stdout.readline().strip()
    assert reply.startswith('OK'), (line, reply)
    return int(reply.split()[1], 0) if len(reply.split()) > 1 else None


def write(reg, value):
    cmd(f'writel 0x40009008 {(reg << 18) | (value << 2):#x}')
    cmd('readl 0x40009008')


def read(reg):
    cmd(f'writel 0x40009008 {(1 << 26) | (reg << 18):#x}')
    return cmd('readl 0x40009008') >> 2


try:
    cmd('writel 0x40009004 0x180')
    write(0x1d, 0x1c)  # Channel4 disconnected: no valid data.
    assert read(0x50) == 0
    write(0x4f, 0x20)
    cmd('clock_step 124999')
    assert read(0x50) == 0, 'conversion must not complete immediately'
    cmd('clock_step 1')
    assert read(0x50) == 0x10c
    assert read(0x58) == 17908 and read(0x59) == 20000
    assert read(0x51) == 0
    write(0x1f, 0xc); write(0x20, 4)
    assert read(0x51) == 4
    write(0x51, 4)
    assert read(0x50) == 0x108
    assert read(0x58) == 17908, 'ACK must not discard latched sample'
    write(0x51, 0x108)
    cmd('clock_step 1000000')
    assert read(0x50) == 0, 'one shot must not manufacture repeated results'
    write(0x18, 0x1000); write(0x1c, 122); write(0x4f, 0x20)
    cmd('clock_step 125000'); assert read(0x50) == 0x10c
    write(0x51, 0x1ff)
    assert read(0x50) == 0
    cmd('clock_step 124928'); assert read(0x50) == 0x10c
    write(0x4f, 0); write(0x51, 0x1ff)
    cmd('clock_step 1000000'); assert read(0x50) == 0
    # Writes to sample/status registers cannot synthesize an input.
    write(0x50, 0xffff); write(0x58, 1)
    assert read(0x50) == 0 and read(0x58) == 17908
    # Different chip/page storage is independent of the PMU ADC.
    write(0, 0xa010); write(0x50, 0xbeef)
    assert read(0x50) == 0xbeef
    write(0, 0xa000); assert read(0x50) == 0
    # Guest AON reset clears runtime conversion state but keeps board inputs.
    cmd('writel 0x400800c8 0x80')
    for _ in range(20):
        if cmd('readl 0x400800c8') == 0:
            break
    else:
        raise AssertionError('reset did not run')
    cmd('writel 0x40009004 0x180')
    assert read(0x50) == 0 and read(0x1d) == 0 and read(0x58) == 0
    write(0x1d, 4); write(0x4f, 0x20); cmd('clock_step 125000')
    assert read(0x58) == 17908 and read(0x50) == 0x104
    print('PASS reference GPADC timing, input isolation, masks, W1C, interval, stop and reset')
finally:
    q.terminate(); q.wait(timeout=5)
    q.stdin.close(); q.stdout.close()
