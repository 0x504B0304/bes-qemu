#!/usr/bin/env python3
"""Validate host-side Xiaomi notifications entering the AP HCI path."""
import os
import select
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from tools.qmp_client import Qmp


def acl(att, handle=1):
    l2cap = bytes((len(att), 0, 4, 0)) + att
    return b'\x02' + struct.pack('<H', handle | 0x2000) + \
        struct.pack('<H', len(l2cap)) + l2cap


class Harness:
    def __init__(self):
        self.error = tempfile.TemporaryFile()
        self.qmp_dir = Path(tempfile.mkdtemp(prefix='miband-xiaomi-peer-'))
        self.endpoint = self.qmp_dir / 'qmp.sock'
        self.process = subprocess.Popen([
            os.environ['MIBAND_QEMU'],
            '-M', 'bes1503,bt-backend=synthetic,bt-synthetic-peer-passive=on',
            '-kernel', os.environ['MIBAND_AP'], '-display', 'none', '-monitor', 'none',
            '-serial', 'none', '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio',
            '-qmp', f'unix:{self.endpoint},server=on,wait=off'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.error)
        self.qmp = Qmp(self.endpoint, self.process)

    def command(self, text):
        self.process.stdin.write((text + '\n').encode())
        self.process.stdin.flush()
        if not select.select([self.process.stdout], [], [], 5)[0]:
            raise TimeoutError(text)
        reply = self.process.stdout.readline().decode().strip().split()
        assert reply and reply[0] == 'OK', (text, reply)
        return reply[1] if len(reply) > 1 else None

    def receive(self):
        self.command('clock_step 10000000')
        node = int(self.command('readl 0xc0004000'), 0)
        assert node, 'synthetic peer did not publish an HCI event'
        length = int(self.command(f'readl {node + 8:#x}'), 0)
        address = int(self.command(f'readl {node + 12:#x}'), 0)
        packet = bytes.fromhex(self.command(f'read {address:#x} {length}')[2:])
        self.command('writel 0x400000a4 1')
        self.command('writel 0xd0330054 4')
        return packet

    def send(self, packet):
        self.command('writel 0xc0004004 0x20010000')
        self.command('writel 0x20010000 0x20010100')
        descriptor = struct.pack('<5I', 0, 0, len(packet), 0x20010200, 1)
        self.command(f'write 0x20010100 20 0x{descriptor.hex()}')
        self.command(f'write 0x20010200 {len(packet)} 0x{packet.hex()}')
        self.command('writel 0x400000a0 0x8004')
        return self.receive()

    def inject(self, packet):
        self.qmp.call('qom-set', {
            'path': '/machine', 'property': 'bt-synthetic-inject',
            'value': packet.hex(),
        })

    def close(self):
        self.qmp.close()
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        self.error.close()
        shutil.rmtree(self.qmp_dir, ignore_errors=True)


def main():
    harness = Harness()
    try:
        peer = bytes.fromhex('a1 a2 a3 a4 a5 c6')
        params = bytes.fromhex('18 00 18 00 00 00') + peer + bytes.fromhex(
            '00 18 00 18 00 00 00 2a 00 00 00 00 00')
        assert harness.send(b'\x01\x0d\x20\x19' + params) == \
            bytes.fromhex('04 0e 04 01 0d 20 00')
        connection = bytes.fromhex(
            '04 3e 13 01 00 01 00 01 00 a1 a2 a3 a4 a5 c6 '
            '18 00 00 00 c8 00 00')
        harness.inject(connection)
        assert harness.receive() == connection
        harness.send(acl(bytes.fromhex('02 40 00')))
        harness.send(acl(bytes.fromhex('10 01 00 06 00 00 28')))
        harness.send(acl(bytes.fromhex('0a 03 00')))

        outgoing = bytes.fromhex('a5 a5 12 07 01 00 c1 c0 01')
        outgoing_acl = acl(bytes.fromhex('12 06 00') + outgoing)
        assert harness.send(outgoing_acl)[9] == 0x13
        assert harness.qmp.call('qom-get', {
            'path': '/machine', 'property': 'bt-synthetic-last-write'}) == outgoing.hex()
        assert harness.qmp.call('qom-get', {
            'path': '/machine', 'property': 'bt-synthetic-last-ap-packet'}) == \
            outgoing_acl.hex()

        incoming = bytes.fromhex('a5 a5 01 07 00 00 00 00')
        incoming_acl = acl(bytes((0x1b, 8, 0)) + incoming)
        harness.inject(incoming_acl)
        assert harness.receive()[12:] == incoming
        assert harness.qmp.call('qom-get', {
            'path': '/machine', 'property': 'bt-synthetic-last-bt-packet'}) == \
            incoming_acl.hex()
        print('XIAOMI_PEER_INJECTION_OK passive=1 outbound_capture=1 inbound_notification=1')
    finally:
        harness.close()


if __name__ == '__main__':
    main()
