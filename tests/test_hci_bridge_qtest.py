#!/usr/bin/env python3
"""Physical PTY framing, FIFO delivery and disconnects through the SoC boundary."""
import os
from pathlib import Path
import pty
import select
import subprocess
import tempfile
import time


class Harness:
    def __init__(self, directory, worker=False):
        self.master, self.slave = pty.openpty()
        self.error = (directory / 'stderr.log').open('w+')
        self.process = subprocess.Popen([os.environ['MIBAND_QEMU'], '-M',
            f'bes1503,bt-backend=serial-btdm:{os.ttyname(self.slave)}@230400,'
            f'bt-rx-flow-control=on,hci-worker={"on" if worker else "off"}',
            '-kernel', os.environ['MIBAND_AP'], '-display', 'none', '-monitor', 'none',
            '-serial', 'none', '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.error, text=True)
        self.cmd('clock_step 1000000')
        os.close(self.slave)
        self.slave = None

    def cmd(self, text):
        self.process.stdin.write(text + '\n')
        self.process.stdin.flush()
        assert select.select([self.process.stdout], [], [], 3)[0], 'qtest stalled'
        reply = self.process.stdout.readline().split()
        assert reply and reply[0] == 'OK', (text, reply)
        return reply[1:]

    def word(self, address):
        return int(self.cmd(f'readl {address:#x}')[0], 0)

    def wait_packet(self, expected):
        deadline = time.monotonic() + 3
        while not self.word(0xc0004000) and time.monotonic() < deadline:
            self.cmd('clock_step 1000000')
            time.sleep(.002)
        node = self.word(0xc0004000)
        assert node, 'complete frame not published to the AP'
        size, data = self.word(node + 8), self.word(node + 12)
        actual = bytes.fromhex(self.cmd(f'read {data:#x} {size}')[0][2:])
        assert actual == expected, (actual.hex(), expected.hex())
        self.cmd('writel 0xd0330054 4')

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        self.error.close()
        if self.master is not None:
            os.close(self.master)
        if self.slave is not None:
            os.close(self.slave)


def main():
    with tempfile.TemporaryDirectory(prefix='bes-hci-bridge-qtest-') as temp:
        for worker in (False, True):
            root = Path(temp) / ('worker' if worker else 'sync')
            root.mkdir()
            harness = Harness(root, worker)
            try:
                first = bytes.fromhex('04 ff 05 11 22 33 44 55')
                # Neither H4 prefix nor a partial payload may be delivered.
                for chunk in (first[:1], first[1:3], first[3:6]):
                    os.write(harness.master, chunk)
                    for _ in range(5):
                        harness.cmd('clock_step 1000000')
                        time.sleep(.003)
                    assert not harness.word(0xc0004000)
                os.write(harness.master, first[6:])
                harness.wait_packet(first)
                second = bytes.fromhex('04 ff 02 66 77')
                third = bytes.fromhex('04 ff 01 88')
                os.write(harness.master, second + third)
                harness.wait_packet(second)
                harness.wait_packet(third)
                for _ in range(5):
                    harness.cmd('clock_step 1000000')
                    time.sleep(.003)
                assert not harness.word(0xc0004000), 'duplicate controller packet'
                # Strict RX cannot recover by guessing where the next packet starts.
                os.write(harness.master, b'\x00')
                harness.process.wait(timeout=5)
                harness.error.flush()
                error = (root / 'stderr.log').read_text()
                assert harness.process.returncode != 0 and ('H4' in error or 'framing' in error), error
                print(f'HCI_BRIDGE_FRAMING_OK worker={int(worker)} fragmented=1 FIFO=1 strict-fault=1')
            finally:
                harness.close()
        root = Path(temp) / 'disconnect'
        root.mkdir()
        harness = Harness(root)
        try:
            os.close(harness.master)
            harness.master = None
            harness.process.wait(timeout=5)
            error = (root / 'stderr.log').read_text()
            assert harness.process.returncode != 0 and 'refusing synthetic fallback' in error, error
            print('HCI_BRIDGE_DISCONNECT_OK fail-closed=1')
        finally:
            harness.close()


if __name__ == '__main__':
    main()
