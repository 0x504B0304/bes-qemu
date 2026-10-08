#!/usr/bin/env python3
"""Verify classic HCI reaches a dual-mode serial backend, including error RX."""
import os
import pty
import select
import signal
import subprocess
import tempfile
import time
from pathlib import Path


def main():
    run = Path(tempfile.mkdtemp(prefix='miband-dual-forward-'))
    master, slave = pty.openpty()
    command = [os.environ['MIBAND_QEMU'], '-M',
               f'bes1503,bt-backend=serial-btdm:{os.ttyname(slave)}@230400',
               '-kernel', os.environ.get('MIBAND_AP', '/mnt/e/HWRE/Miband/vela_ap.bin'),
               '-display', 'none', '-monitor', 'none', '-serial', 'none',
               '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio',
               '-d', 'unimp', '-D', str(run / 'qemu.log')]
    stderr = (run / 'stderr.log').open('w')
    proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=stderr, text=True)

    def cmd(line):
        proc.stdin.write(line + '\n')
        proc.stdin.flush()
        result = proc.stdout.readline().strip()
        assert result.startswith('OK'), (line, result)
        return result.split()[1] if len(result.split()) > 1 else None

    def word(address):
        return int(cmd(f'readl {address:#x}'), 0)

    def timeout(*_):
        raise TimeoutError('dual-mode forwarding test stalled')

    signal.signal(signal.SIGALRM, timeout)
    signal.alarm(25)
    try:
        for addr, value in [(0xc0004004, 0x20010000), (0x20010000, 0x20010100),
                            (0x20010100, 0), (0x20010104, 0),
                            (0x2001010c, 0x20010200), (0x20010110, 1)]:
            cmd(f'writel {addr:#x} {value:#x}')
        packets = [bytes.fromhex('01 1a 0c 01 03'),   # classic Write Scan Enable
                   bytes.fromhex('01 14 0c 00'),      # classic Read Local Name
                   bytes.fromhex('01 35 0c 05 01 01 00 02 00')]  # flow credits
        for packet in packets:
            cmd(f'write 0x20010200 {len(packet)} 0x{packet.hex()}')
            cmd(f'writel 0x20010108 {len(packet)}')
            cmd('writel 0x400000a0 0x8004')
            got = bytearray()
            end = time.monotonic() + 3
            while len(got) < len(packet) and time.monotonic() < end:
                cmd('clock_step 1000000')
                if select.select([master], [], [], 0.02)[0]:
                    got.extend(os.read(master, 4096))
            assert got == packet, (packet.hex(), got.hex())
            cmd('writel 0x400000a4 1')
            # Host Number Of Completed Packets deliberately has no completion.
            if packet[1:3] == b'5\x0c':
                continue
            # A real controller rejection must not be changed to success.
            event = bytes.fromhex('04 0e 04 01') + packet[1:3] + b'\x12'
            os.write(master, event)
            end = time.monotonic() + 3
            while not word(0xc0004000) and time.monotonic() < end:
                cmd('clock_step 1000000')
                time.sleep(0.01)
            desc = word(0xc0004000)
            assert desc, 'controller response not published to AP'
            length = word(desc + 8)
            ptr = word(desc + 12)
            received = bytes.fromhex(cmd(f'read {ptr:#x} {length}')[2:])
            assert received == event, (received.hex(), event.hex())
            cmd('writel 0xd0330054 4')
        print(f'DUAL_FORWARD_OK classic=2 credits=1 real-error-preserved=1 logs={run}')
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
        stderr.close()
        os.close(master)
        os.close(slave)


if __name__ == '__main__':
    main()
