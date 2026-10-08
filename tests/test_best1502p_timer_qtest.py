#!/usr/bin/env python3
"""Exercise BEST1502P timer count, expiry, IRQ masking/clear and rearm."""
import argparse
import subprocess

p = argparse.ArgumentParser()
p.add_argument('qemu')
p.add_argument('ap_image')
a = p.parse_args()
q = subprocess.Popen([a.qemu, '-M', 'miband-best1502p', '-kernel', a.ap_image,
    '-display', 'none', '-monitor', 'none', '-accel', 'qtest', '-qtest', 'stdio',
    '-serial', 'none', '-serial', 'none'], stdin=subprocess.PIPE,
    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)

irqs = {}

def cmd(s):
    q.stdin.write(s+'\n'); q.stdin.flush()
    r = q.stdout.readline().strip()
    while r.startswith('IRQ '):
        _, state, number = r.split()
        irqs[int(number)] = state == 'raise'
        r = q.stdout.readline().strip()
    assert r.startswith('OK'), (s,r)
    return int(r.split()[-1],0) if len(r.split()) > 1 else None

try:
    # reference's CPU-load timer selects the external 16 kHz SysTick source.
    # LOAD=15 must not wrap at 0.5 ms and must wrap just after 1 ms.
    cmd('writel 0xe000e014 15')
    cmd('writel 0xe000e018 0')
    cmd('writel 0xe000e010 1')
    cmd('clock_step 500000')
    assert not (cmd('readl 0xe000e010') & 0x10000)
    cmd('clock_step 510000')
    assert cmd('readl 0xe000e010') & 0x10000
    cmd('writel 0xe000e010 0')
    cmd('irq_intercept_in /machine/armv7m')
    cmd('writel 0x40084008 0x82')
    before = cmd('readl 0x40084004')
    cmd('clock_step 1000000000')
    assert (before-cmd('readl 0x40084004')) & 0xffffffff == 16384
    cmd('writel 0x40003008 0x82')
    before = cmd('readl 0x40003004')
    cmd('clock_step 1000000000')
    assert (before-cmd('readl 0x40003004')) & 0xffffffff == 6000000
    cmd('writel 0x40084020 160')
    cmd('writel 0x40084028 0xa3')
    cmd('clock_step 5000000')
    assert cmd('readl 0x40084024') == 80
    assert cmd('readl 0x40084030') == 0
    cmd('clock_step 5100000')
    assert cmd('readl 0x40084030') == 1
    assert cmd('readl 0x40084034') == 1
    assert irqs.get(46, False), 'IRQ 46 must reach NVIC'
    cmd('writel 0x40084028 0x03')
    assert cmd('readl 0x40084030') == 1
    assert cmd('readl 0x40084034') == 0
    cmd('writel 0x4008402c 1')
    assert cmd('readl 0x40084030') == 0
    cmd('writel 0xe000e284 0x4000')
    assert not irqs.get(46, False)
    # Rearming must raise another interrupt, independent of the cleared latch.
    cmd('writel 0x40084020 16')
    cmd('writel 0x40084028 0xa3')
    cmd('clock_step 1100000')
    assert cmd('readl 0x40084034') == 1
    assert irqs.get(46, False)
    print('PASS SysTick 16kHz, counters, alarm, IRQ46, mask, clear, rearm')
finally:
    q.terminate()
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill(); q.wait()
