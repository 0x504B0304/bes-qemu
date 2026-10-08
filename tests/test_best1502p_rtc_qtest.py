#!/usr/bin/env python3
"""reference PMU calendar: real MMIO, BCD commit, clock selection and AP reset."""
import argparse
import select
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('qemu')
p.add_argument('ap_image')
a = p.parse_args()


def verify(clock):
    q = subprocess.Popen([a.qemu, '-M', 'miband-best1502p',
        '-kernel', a.ap_image, '-display', 'none', '-monitor', 'none',
        '-serial', 'none', '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio',
        '-rtc', f'base=2026-09-30T23:59:58,clock={clock}'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True)

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

    def page(cs, index):
        cmd(f'writel 0x40009004 {cs << 7:#x}')
        write(0, 0xa000 + 16 * index)

    def calendar():
        page(3, 1)
        return tuple(read(r) for r in (0x16, 0x15, 0x14, 0x13))

    try:
        page(3, 0)
        assert read(0x61) & 1
        if clock == 'vm':
            assert calendar() == (0x26, 0x8930, 0x23, 0x5958)
            cmd('clock_step 3000000000')
            assert calendar() == (0x26, 0xb001, 0, 1), calendar()
        # Load a leap-day boundary. Staging must not alter running time.
        before = calendar()
        for reg, value in ((0x12, 0x24), (0x11, 0x8228), (0x10, 0x23), (0x0f, 0x5959)):
            write(reg, value)
        assert calendar()[:3] == before[:3]
        write(0x0e, 1)
        assert read(0x0e) & 1 == 0
        assert calendar()[:3] == (0x24, 0x8228, 0x23)
        if clock != 'vm':
            cmd('clock_step 120000000000')
            assert calendar()[1] == 0x8228, 'host/rt followed virtual clock'
            time.sleep(2.1)
        else:
            cmd('clock_step 2000000000')
        assert calendar()[:3] == (0x24, 0xa229, 0), calendar()
        for reg, value in ((0x12, 0x25), (0x11, 0xa229), (0x0f, 0x596a)):
            # Invalid BCD and non-leap Feb29 must not replace the calendar.
            write(reg, value)
        before = calendar()
        write(0x0e, 1)
        assert calendar()[:3] == before[:3]
        write(0x0f, 0)
        write(0x0e, 1)
        assert calendar()[:3] == before[:3]
        # Readback registers are read-only; unrelated CS/page remain storage.
        for reg in (0x13, 0x14, 0x15, 0x16):
            write(reg, 0xffff)
        assert calendar()[:3] == before[:3]
        page(2, 1); write(0x16, 0xabcd); assert read(0x16) == 0xabcd
        page(3, 0); write(0x16, 0x1234); assert read(0x16) == 0x1234
        # AON AP reset clears transport/staging but preserves calendar domain.
        cmd('writel 0x400800c8 0x80')
        for _ in range(30):
            if cmd('readl 0x400800c8') == 0:
                break
        else:
            raise AssertionError('AON reset did not run')
        assert calendar()[:3] == before[:3]
        assert read(0x12) == 0
        page(3, 0); assert read(0x61) & 1
        print(f'PASS reference RTC {clock}: BCD/date/load, rollover, isolation, AP reset')
    finally:
        q.terminate(); q.wait(timeout=5)
        q.stdin.close(); q.stdout.close()


for source in ('vm', 'host', 'rt'):
    verify(source)
