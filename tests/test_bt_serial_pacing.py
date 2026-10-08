#!/usr/bin/env python3
"""Actual QEMU/PTTY test: ordered paced writes and concurrent real RX."""
import os
from pathlib import Path
import pty
import re
import select
import subprocess
import tempfile
import time

with tempfile.TemporaryDirectory(prefix='mb-pacing-') as directory:
    path = Path(directory)
    master, slave = pty.openpty()
    with (path/'stderr').open('w') as err:
        proc = subprocess.Popen([os.environ['MIBAND_QEMU'], '-M',
            f'bes1503,bt-backend=serial-btdm:{os.ttyname(slave)}@230400,bt-serial-tx-rate=12800',
            '-kernel', os.environ.get('MIBAND_AP','/mnt/e/HWRE/Miband/vela_ap.bin'),
            '-display','none','-monitor','none','-serial','none','-serial','none',
            '-accel','qtest','-qtest','stdio','-d','unimp','-D',str(path/'qemu.log')],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err, text=True)
        def cmd(line):
            proc.stdin.write(line+'\n'); proc.stdin.flush()
            assert select.select([proc.stdout],[],[],3)[0], 'qtest timeout'
            reply = proc.stdout.readline().strip()
            assert reply.startswith('OK'), reply
            return reply.split()[1:]
        def word(address): return int(cmd(f'readl {address:#x}')[0],0)
        try:
            for address,value in [(0xc0004004,0x20010000),(0x20010000,0x20010100),
                    (0x20010100,0),(0x20010104,0),(0x2001010c,0x20010200),(0x20010110,1)]:
                cmd(f'writel {address:#x} {value:#x}')
            packets = [b'\x02\x80\x20'+(1021).to_bytes(2,'little')+bytes(range(256))*3+bytes(range(253)),
                       bytes.fromhex('01 14 0c 00'),
                       b'\x02\x80\x20'+(512).to_bytes(2,'little')+bytes(range(255,-1,-1))*2]
            for packet in packets:
                cmd(f'write 0x20010200 {len(packet)} 0x{packet.hex()}')
                cmd(f'writel 0x20010108 {len(packet)}')
                cmd('writel 0x400000a0 0x8004'); cmd('clock_step 1000000')
                cmd('writel 0x400000a4 1')
            # RX must progress while the paced TX FIFO is still draining.
            event = bytes.fromhex('04 0e 04 01 14 0c 12')
            os.write(master,event)
            expected = b''.join(packets)
            got = bytearray()
            deadline = time.monotonic()+4
            rx_at_bytes = None
            while (len(got)<len(expected) or rx_at_bytes is None) and time.monotonic()<deadline:
                cmd('clock_step 1000000')
                if select.select([master],[],[],.001)[0]: got.extend(os.read(master,4096))
                pointer = word(0xc0004000)
                if pointer and rx_at_bytes is None:
                    size, data = word(pointer+8), word(pointer+12)
                    assert bytes.fromhex(cmd(f'read {data:#x} {size}')[0][2:]) == event
                    rx_at_bytes = len(got)
            assert got == expected, (len(got),len(expected))
            assert rx_at_bytes is not None and rx_at_bytes < len(expected), rx_at_bytes
            # No synthetic response / duplicate byte follows the exact stream.
            assert not select.select([master],[],[],.02)[0]
            records = [(int(n),int(t)) for n,t in re.findall(
                r'serial paced bytes=(\d+) rate=12800 time-ns=(\d+)', (path/'qemu.log').read_text())]
            assert sum(n for n,t in records)==len(expected)
            assert all(0<n<=64 for n,t in records)
            for (n,t),(nn,tt) in zip(records,records[1:]):
                assert tt-t >= (n*1000000000+12799)//12800, (n,tt-t)
            print(f'PASS: {len(expected)} bytes FIFO exact, {len(records)} paced writes <=64 B; spacing enforced; RX during TX at byte {rx_at_bytes}')
        finally:
            proc.terminate(); proc.wait(timeout=3)
            os.close(master); os.close(slave)
