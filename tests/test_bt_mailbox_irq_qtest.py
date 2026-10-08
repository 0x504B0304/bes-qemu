#!/usr/bin/env python3
"""Check latched mailbox IRQs without executing or patching the AP firmware.

Reproduce a controller reply arriving while the AP RX mask is off, as happens
when the boot driver closes intersys and the NuttX driver reopens it. A pulse
must not substitute for the level that remains asserted until acknowledgement.
"""
import os
import signal
import subprocess


def main():
    qemu = os.environ.get('MIBAND_QEMU', '/tmp/miband-qemu-handoff-20260930/build/qemu-system-arm')
    ap = os.environ.get('MIBAND_AP', '/mnt/e/HWRE/Miband/vela_ap.bin')
    proc = subprocess.Popen(
        [qemu, '-M', 'bes1503', '-kernel', ap, '-display', 'none',
         '-monitor', 'none', '-serial', 'none', '-serial', 'none',
         '-accel', 'qtest', '-qtest', 'stdio'],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True)
    levels = {}

    def cmd(line):
        proc.stdin.write(line + '\n')
        proc.stdin.flush()
        while True:
            reply = proc.stdout.readline().strip()
            if reply.startswith('IRQ '):
                _, action, irq = reply.split()
                levels[int(irq)] = action == 'raise'
                continue
            assert reply.startswith('OK'), (line, reply)
            return int(reply.split()[1], 0) if len(reply.split()) > 1 else 0

    def irq(number):
        cmd('readl 0x400000a0')  # synchronize any pending QTest notification
        return levels.get(number, False)

    def timeout(*_):
        raise TimeoutError('mailbox QTest stalled')

    signal.signal(signal.SIGALRM, timeout)
    signal.alarm(20)
    try:
        cmd('irq_intercept_in /machine/armv7m')
        # One HCI Reset descriptor in guest RAM, as passed by intersys_send.
        for addr, val in [(0xc0004004, 0x20010000), (0x20010000, 0x20010100),
                          (0x20010100, 0), (0x20010104, 0), (0x20010108, 4),
                          (0x2001010c, 0x20010200), (0x20010110, 1),
                          (0x20010200, 0x000c0301)]:
            cmd(f'writel {addr:#x} {val:#x}')
        cmd('writel 0x400000a0 0x8004')  # TX enable + submit; RX masked
        cmd('clock_step 1')
        assert irq(32), 'TX done must remain high until AP clears it'
        assert not irq(34), 'RX must stay low while masked'
        cmd('writel 0x400000a4 1')
        assert not irq(32), 'TX acknowledgement must lower IRQ'
        cmd('clock_step 10000000')
        assert cmd('readl 0xc0004000') != 0, 'reply must remain pending while masked'
        assert not irq(34)
        cmd('writel 0x400000a0 0x2000')
        assert irq(34), 'unmask must assert a previously pending reply'
        cmd('clock_step 10000000')
        assert irq(34), 'time passing must not turn the interrupt into a pulse'
        cmd('writel 0x400000a4 0x2000')
        assert not irq(34), 'mask must lower line without losing pending reply'
        cmd('writel 0x400000a0 0x2000')
        assert irq(34), 'unmask must reassert the same unconsumed reply'
        cmd('writel 0xd0330054 4')
        assert not irq(34), 'controller RX acknowledgement must lower line'
        assert cmd('readl 0xc0004000') == 0
        print('BT_MAILBOX_IRQ_OK tx-held=1 masked-rx=1 unmask-reassert=1 ack-clears=1')
    finally:
        signal.alarm(0)
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        proc.stdin.close()
        proc.stdout.close()


if __name__ == '__main__':
    main()
