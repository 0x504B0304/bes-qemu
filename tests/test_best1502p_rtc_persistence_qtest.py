#!/usr/bin/env python3
"""Real PMU writes, abrupt/graceful restart, old journals and corrupt state."""
import argparse
import hashlib
import json
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import time
import zlib


class Machine:
    def __init__(self, args, directory, clock='vm'):
        self.directory = directory
        self.store = directory / 'flash.journal'
        self.rtc = directory / 'flash.journal.rtc'
        self.qmp = directory / 'qmp.sock'
        self.command = [args.qemu, '-M',
            f'miband-best1502p,resource-image={args.resource},flash-state={self.store},'
            'nand-id=0xc8f4c8,nand-page-size=2048,nand-spare-size=64,'
            'nand-page-count=262144,nand-pages-per-block=64',
            '-kernel', args.ap, '-display', 'none', '-monitor', 'none',
            '-serial', 'none', '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio',
            '-qmp', f'unix:{self.qmp},server=on,wait=off',
            '-rtc', f'base=2026-09-30T23:59:58,clock={clock}']
        self.process = None
        self.error = (directory / 'stderr').open('w+')

    def start(self):
        self.process = subprocess.Popen(self.command, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.error, text=True)
        self.cmd('readl 0x40009004')

    def stop(self, graceful=False):
        if self.process and self.process.poll() is None:
            if graceful:
                with socket.socket(socket.AF_UNIX) as sock:
                    sock.settimeout(5)
                    sock.connect(str(self.qmp))
                    with sock.makefile('rwb', buffering=0) as stream:
                        assert 'QMP' in json.loads(stream.readline())
                        for command in ('qmp_capabilities', 'quit'):
                            stream.write(json.dumps({'execute': command}).encode() + b'\n')
                            while True:
                                reply = json.loads(stream.readline())
                                if 'return' in reply:
                                    break
                                assert 'error' not in reply, reply
            else:
                self.process.kill()
            self.process.wait(timeout=10)
        if self.process:
            self.process.stdin.close()
            self.process.stdout.close()

    def cmd(self, line):
        self.process.stdin.write(line + '\n')
        self.process.stdin.flush()
        assert select.select([self.process.stdout], [], [], 10)[0], 'QTest timeout'
        reply = self.process.stdout.readline().strip()
        assert reply.startswith('OK'), (line, reply,
            self.rtc.read_bytes().hex() if self.rtc.exists() else None,
            (self.directory / 'stderr').read_text()[-2000:])
        return int(reply.split()[1], 0) if len(reply.split()) > 1 else None

    def write(self, reg, value):
        self.cmd(f'writel 0x40009008 {(reg << 18) | (value << 2):#x}')
        self.cmd('readl 0x40009008')

    def read(self, reg):
        self.cmd(f'writel 0x40009008 {(1 << 26) | (reg << 18):#x}')
        return self.cmd('readl 0x40009008') >> 2

    def calendar(self):
        self.cmd('writel 0x40009004 0x180')
        self.write(0, 0xa010)
        return tuple(self.read(r) for r in (0x16, 0x15, 0x14, 0x13))

    def load(self):
        self.calendar()
        for reg, value in ((0x12, 0x28), (0x11, 0xc629), (0x10, 0x12), (0x0f, 0x3456)):
            self.write(reg, value)
        self.write(0x0e, 1)

    def rejected(self, reason):
        result = subprocess.run(self.command, input='', capture_output=True,
                                text=True, timeout=10)
        assert result.returncode != 0 and reason in result.stderr, result.stderr


def verify(args, clock):
    with tempfile.TemporaryDirectory(prefix='soc-rtc-state-') as name:
        m = Machine(args, Path(name), clock)
        try:
            m.start()
            original_journal = hashlib.sha256(m.store.read_bytes()).hexdigest()
            m.load()
            assert m.calendar()[:3] == (0x28, 0xc629, 0x12)
            # LOAD must be durable immediately, without graceful shutdown.
            m.stop()
            if clock != 'vm':
                time.sleep(2.1)
            m.start()
            assert m.calendar()[:3] == (0x28, 0xc629, 0x12)
            value = m.calendar()[3]
            if clock != 'vm':
                assert 0x3458 <= value <= 0x3505, value
            else:
                assert value == 0x3456, value
                m.cmd('clock_step 3500000000')
                assert m.calendar()[3] == 0x3459
                m.stop(); m.start()
                assert m.calendar()[3] == 0x3459
                # SIGKILL restores the last whole-second checkpoint. A new
                # fractional second survives normal quit, which saves even
                # when the one-second checkpoint has not fired yet.
                m.cmd('clock_step 250000000')
                m.stop(graceful=True); m.start()
                m.cmd('clock_step 750000000')
                assert m.calendar()[3] == 0x3500, (m.calendar(), m.rtc.read_bytes().hex())
                before = m.rtc.read_bytes()
                m.write(0x0f, 0x596a); m.write(0x0e, 1)
                assert m.rtc.read_bytes() == before, 'invalid LOAD persisted'
                m.rejected('state is locked')
            m.stop()
            assert hashlib.sha256(m.store.read_bytes()).hexdigest() == original_journal
            # Invalid snapshots are retained for diagnosis, never silently reset.
            good = m.rtc.read_bytes()
            for bad in (good[:-1], good + b'x', b'X' + good[1:]):
                m.rtc.write_bytes(bad)
                m.rejected('invalid calendar')
                assert m.rtc.read_bytes() == bad
            for offset in (8, 56, 60):
                bad = bytearray(good)
                bad[offset] ^= 0xff
                struct.pack_into('<I', bad, 64, zlib.crc32(bad[:64]))
                m.rtc.write_bytes(bad)
                m.rejected('invalid calendar')
            # A historical journal without a sidecar seeds from -rtc. No
            # format migration or NAND rewrite is required.
            m.rtc.unlink()
            m.start()
            assert m.calendar()[:3] == (0x26, 0x8930, 0x23)
            m.stop()
            assert hashlib.sha256(m.store.read_bytes()).hexdigest() == original_journal
            print(f'PASS reference RTC durable {clock}: LOAD, restart, validation, old journal')
        finally:
            m.stop()
            m.error.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for key in ('qemu', 'ap', 'resource'):
        parser.add_argument('--' + key, required=True)
    args = parser.parse_args()
    for clock in ('vm', 'host', 'rt'):
        verify(args, clock)
