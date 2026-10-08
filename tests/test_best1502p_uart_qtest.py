#!/usr/bin/env python3
"""Qualify reference UART RX -> NVIC routing, masking and FIFO consumption."""
import argparse
from pathlib import Path
import socket
import subprocess
import tempfile
import time

p = argparse.ArgumentParser()
p.add_argument('qemu')
p.add_argument('ap_image')
a = p.parse_args()

with tempfile.TemporaryDirectory(prefix='soc-uart-') as temp:
    paths = [str(Path(temp) / f'uart{i}.sock') for i in range(2)]
    command = [a.qemu, '-M', 'miband-best1502p', '-kernel', a.ap_image,
               '-display', 'none', '-monitor', 'none', '-accel', 'qtest', '-qtest', 'stdio']
    for path in paths:
        command += ['-serial', f'unix:{path},server=on,wait=off']
    q = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True)
    irqs = {}
    sockets = []

    def cmd(s):
        q.stdin.write(s + '\n'); q.stdin.flush()
        while True:
            reply = q.stdout.readline().strip()
            if reply.startswith('IRQ '):
                _, state, number = reply.split()
                irqs[int(number)] = state == 'raise'
            else:
                assert reply.startswith('OK'), (s, reply)
                return int(reply.split()[-1], 0) if len(reply.split()) > 1 else None

    try:
        cmd('irq_intercept_in /machine/armv7m')
        for path in paths:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(path); sockets.append(sock)
        for index, (base, irq) in enumerate(((0x4000c000, 18), (0x4000d000, 26))):
            cmd(f'writel {base+0x30:#x} 0x301')  # UART/RX/TX enabled
            cmd(f'writel {base+0x2c:#x} 0x60')   # 8-bit, FIFO off: threshold one byte
            cmd(f'writel {base+0x38:#x} 0')
            sockets[index].sendall(b'Q')
            deadline = time.monotonic() + 3
            while cmd(f'readl {base+0x18:#x}') & 0x10:
                assert time.monotonic() < deadline, 'RX did not reach FIFO'
                time.sleep(.01)
            assert cmd(f'readl {base+0x3c:#x}') & 0x10
            assert not irqs.get(irq, False), 'masked RX must not interrupt'
            cmd(f'writel {base+0x38:#x} 0x10')
            assert cmd(f'readl {base+0x40:#x}') & 0x10
            assert irqs.get(irq, False), f'RX must reach NVIC IRQ{irq}'
            cmd(f'writel {base+0x44:#x} 0x10')
            assert cmd(f'readl {base+0x40:#x}') & 0x10, 'ICR must not strand unread RX'
            assert irqs.get(irq, False), 'RX remains a level until FIFO drained'
            assert cmd(f'readl {base:#x}') == ord('Q')
            assert cmd(f'readl {base+0x18:#x}') & 0x10
            assert not irqs.get(irq, False), 'consuming RX must lower IRQ'
        print('PASS UART1/IRQ18 + UART2/IRQ26 RX, mask, pending and drain')
    finally:
        for sock in sockets: sock.close()
        q.terminate(); q.wait(timeout=5)
        q.stdin.close(); q.stdout.close()
