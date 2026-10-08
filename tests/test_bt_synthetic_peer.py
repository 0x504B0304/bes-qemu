#!/usr/bin/env python3
"""Validate the synthetic LE peer through the real BEST1503 mailbox."""
import os
import select
import struct
import subprocess
import tempfile


def hci(opcode, params=b''):
    return b'\x01' + opcode.to_bytes(2, 'little') + bytes([len(params)]) + params


def acl(att, handle=1):
    l2cap = bytes([len(att), 0, 4, 0]) + att
    return b'\x02' + struct.pack('<H', handle | 0x2000) + \
        struct.pack('<H', len(l2cap)) + l2cap


class Harness:
    def __init__(self):
        self.error = tempfile.TemporaryFile()
        self.process = subprocess.Popen([
            os.environ['MIBAND_QEMU'], '-M', 'bes1503,bt-backend=synthetic',
            '-kernel', os.environ['MIBAND_AP'], '-display', 'none', '-monitor', 'none',
            '-serial', 'none', '-serial', 'none', '-accel', 'qtest', '-qtest', 'stdio'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.error)

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

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=5)
        self.process.stdin.close()
        self.process.stdout.close()
        self.error.close()


def main():
    harness = Harness()
    try:
        peer = bytes.fromhex('a1 a2 a3 a4 a5 c6')
        params = bytes.fromhex('18 00 18 00 00 00') + peer + bytes.fromhex(
            '00 18 00 18 00 00 00 2a 00 00 00 00 00')
        assert len(params) == 25
        complete = bytes.fromhex('04 0e 04 01 0d 20 00')
        assert harness.send(hci(0x200d, params)) == complete
        assert harness.receive() == bytes.fromhex(
            '04 3e 13 01 00 01 00 00 00 a1 a2 a3 a4 a5 c6 '
            '18 00 00 00 2a 01 00')
        assert harness.send(acl(bytes.fromhex('02 40 00'))) == \
        bytes.fromhex('02 01 20 07 00 03 00 04 00 03 40 00')
        assert harness.send(acl(bytes.fromhex('10 01 00 06 00 00 28'))) == \
            bytes.fromhex('02 01 20 12 00 0e 00 04 00 11 06 01 00 03 00 00 18 04 00 09 00 95 fe')
        assert harness.send(acl(bytes.fromhex('0a 03 00'))) == \
            bytes.fromhex('02 01 20 16 00 12 00 04 00 0b 58 69 61 6f 6d 69'
                          '20 53 6d 61 72 74 20 42 61 6e 64')
        # Xiaomi L1 command on TX characteristic 005f gets a protocol ACK on
        # RX characteristic 005e after the ATT Write Response.
        l1_command = bytes.fromhex('a5 a5 12 07 01 00 c1 c0 01')
        assert harness.send(acl(bytes.fromhex('12 06 00') + l1_command)) == \
            bytes.fromhex('02 01 20 05 00 01 00 04 00 13')
        assert harness.receive() == bytes.fromhex(
            '02 01 20 0f 00 0b 00 04 00 1b 08 00 a5 a5 01 07 00 00 00 00')
        l1_data = bytes.fromhex('a5 a5 13 08 03 00 20 2f 02 01 aa')
        assert harness.send(acl(bytes.fromhex('52 06 00') + l1_data)) == \
            bytes.fromhex('02 01 20 0f 00 0b 00 04 00 1b 08 00 a5 a5 01 08 00 00 00 00')
        assert harness.send(hci(0x0406, bytes.fromhex('01 00 13'))) == \
            bytes.fromhex('04 0e 04 01 06 04 00')
        assert harness.receive() == bytes.fromhex('04 05 04 00 01 00 13')
        print('SYNTHETIC_LE_PEER_OK connection=1 disconnect=1 deterministic=1')
    finally:
        harness.close()


if __name__ == '__main__':
    main()
