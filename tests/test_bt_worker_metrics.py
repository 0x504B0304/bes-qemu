#!/usr/bin/env python3
"""QEMU/PTTY test of receive, publication and one-time mailbox-ACK metrics."""
import os
from pathlib import Path
import pty
import re
import select
import subprocess
import tempfile
import time


def main():
    qemu = os.environ.get('MIBAND_QEMU',
                          '/tmp/miband-qemu-parallel-20260930/build/qemu-system-arm')
    directory = Path(tempfile.mkdtemp(prefix='miband-hci-metrics-'))
    master, slave = pty.openpty()
    with (directory / 'stderr').open('w') as err:
        proc = subprocess.Popen([
            qemu, '-M', f'bes1503,bt-backend=serial-btdm:{os.ttyname(slave)}@921600,hci-worker=on',
            '-kernel', os.environ.get('MIBAND_AP', '/mnt/e/HWRE/Miband/vela_ap.bin'),
            '-display', 'none', '-monitor', 'none', '-serial', 'none', '-serial', 'none',
            '-accel', 'qtest', '-qtest', 'stdio', '-d', 'unimp', '-D', str(directory / 'qemu.log')],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err, text=True)

        def cmd(line):
            proc.stdin.write(line + '\n')
            proc.stdin.flush()
            assert select.select([proc.stdout], [], [], 3)[0], 'qtest timeout'
            result = proc.stdout.readline().strip()
            assert result.startswith('OK'), (line, result, directory)
            return result.split()[1:]

        def metrics(expected):
            deadline = time.monotonic() + 4
            while time.monotonic() < deadline:
                cmd('readl 0xc0004000')  # Service the main loop without advancing virtual time.
                records = [line for line in (directory / 'qemu.log').read_text().splitlines()
                           if line.startswith('miband BT worker epoch=')]
                if records:
                    fields = {key: int(value) for key, value in
                              re.findall(r'(\S+)=(\d+)', records[-1])}
                    if all(fields.get(key) == value for key, value in expected.items()):
                        return fields
                time.sleep(.02)
            raise AssertionError((expected, records[-1:] if records else [], directory))

        def published():
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                cmd('clock_step 1000000')
                pointer = int(cmd('readl 0xc0004000')[0], 0)
                if pointer:
                    return pointer
                time.sleep(.002)
            raise AssertionError('no mailbox publication')

        try:
            cmd('readl 0xc0004000')  # Confirm startup/termios setup before PTY injection.
            # A valid private diagnostic completion is admitted but absorbed.
            internal = bytes((4, 14, 97, 1, 2, 252, 0, 1)) + bytes(92)
            os.write(master, internal)
            metrics(dict(admitted=1, **{'internal-consumed': 1}, publications=0, acknowledged=0))
            first, second = bytes((4, 0xff, 1, 0x51)), bytes((4, 0xff, 1, 0x52))
            os.write(master, first + second)
            pointer = published()
            data = int(cmd(f'readl {pointer + 12:#x}')[0], 0)
            assert bytes.fromhex(cmd(f'read {data:#x} 4')[0][2:]) == first
            metrics(dict(admitted=3, **{'internal-consumed': 1}, publications=1, acknowledged=0))
            # The second reply is waiting behind the first publication. ACK
            # releases only that live slot; duplicate ACKs cannot create samples.
            for _ in range(5):
                cmd('writel 0xd0330054 4')
            metrics(dict(admitted=3, publications=1, acknowledged=1,
                         **{'rx-publications': 1, 'rx-acknowledged': 1}))
            pointer = published()
            data = int(cmd(f'readl {pointer + 12:#x}')[0], 0)
            assert bytes.fromhex(cmd(f'read {data:#x} 4')[0][2:]) == second
            metrics(dict(admitted=3, publications=2, acknowledged=1))
            for _ in range(5):
                cmd('writel 0xd0330054 4')
            metrics(dict(admitted=3, publications=2, acknowledged=2,
                         **{'rx-publications': 2, 'rx-acknowledged': 2}))
            records = [line for line in (directory / 'qemu.log').read_text().splitlines()
                       if line.startswith('miband BT latency epoch=')]
            latency = {key: int(value) for key, value in re.findall(r'(\S+)=(\d+)', records[-1])}
            for stage in ('admit', 'read-to-publication', 'publication-to-ack', 'read-to-ack', 'commit'):
                assert latency[stage + '-max'] > 0
                assert 0 < latency[stage + '-p50'] <= latency[stage + '-p95'] <= latency[stage + '-p99']
            assert latency['read-to-ack-max'] >= latency['publication-to-ack-max']
            assert latency['read-to-ack-max'] >= latency['read-to-publication-max']
            print(f'PASS admitted=3/internal=1/publications=2/mailbox-ACK=2; '
                  f'duplicate ACKs ignored, five latency stages; evidence={directory}')
        finally:
            if proc.poll() is None:
                proc.terminate()
            proc.wait(timeout=3)
            proc.stdin.close()
            proc.stdout.close()
            os.close(master)
            os.close(slave)


if __name__ == '__main__':
    main()
