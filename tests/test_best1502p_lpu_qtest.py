#!/usr/bin/env python3
"""CMU readiness must follow requests/time, not writes to status or polling."""
import argparse
import select
import subprocess

p = argparse.ArgumentParser()
p.add_argument('qemu')
p.add_argument('ap_image')
a = p.parse_args()
q = subprocess.Popen([a.qemu, '-M', 'miband-best1502p', '-kernel', a.ap_image,
    '-display', 'none', '-monitor', 'none', '-serial', 'none', '-serial', 'none',
    '-accel', 'qtest', '-qtest', 'stdio'], stdin=subprocess.PIPE,
    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)


def cmd(line):
    q.stdin.write(line + '\n'); q.stdin.flush()
    assert select.select([q.stdout], [], [], 5)[0], 'QTest timeout'
    reply = q.stdout.readline().strip()
    assert reply.startswith('OK'), (line, reply)
    return int(reply.split()[1], 0) if len(reply.split()) > 1 else None


def write(value): cmd(f'writel 0x4000008c {value:#x}')
def read(): return cmd('readl 0x4000008c')


try:
    assert read() == 0
    write(0xc000)
    cmd('clock_step 1000000000')
    assert read() == 0, 'status cannot be written or become ready without request'
    # Actual firmware boot sequence; changing the unused PLL timer does not
    # restart the oscillator timer, and a stable read does not advance time.
    write(0x1040)
    for _ in range(20): assert read() == 0x1040
    cmd('clock_step 60000'); write(0x1740)
    cmd('clock_step 1939999'); assert read() == 0x1740
    cmd('clock_step 1'); assert read() == 0x5740
    # A repeated write, even echoing read-only status, keeps readiness.
    write(read()); assert read() == 0x5740
    # PLL enable has its own programmed wait; oscillator stays ready.
    write(0x3740)
    cmd('clock_step 218749'); assert read() == 0x7740
    cmd('clock_step 1'); assert read() == 0xf740
    # Disabling cancels pending/completed readiness.
    write(0x740); assert read() == 0x740
    write(0x1002); cmd('clock_step 30000'); write(0)
    cmd('clock_step 1000000'); assert read() == 0
    # Timer reprogramming starts the new wait, not the old deadline.
    write(0x1002); cmd('clock_step 30000'); write(0x1004)
    cmd('clock_step 124999'); assert read() == 0x1004
    cmd('clock_step 1'); assert read() == 0x5004
    # Reset removes pending/completed state, without altering another port.
    cmd('writel 0x40000088 0x1234')
    cmd('writel 0x400800c8 0x80')
    for _ in range(30):
        if cmd('readl 0x400800c8') == 0: break
    else: raise AssertionError('reset did not run')
    assert read() == 0
    assert cmd('readl 0x40000088') == 0x1234
    write(0x1000); cmd('clock_step 31249'); assert read() == 0x1000
    cmd('clock_step 1'); assert read() == 0x5000
    print('PASS reference LPU request/timing/status isolation/cancellation/reset')
finally:
    q.terminate(); q.wait(timeout=5)
    q.stdin.close(); q.stdout.close()
