"""Synthetic AP and backend tests for the optional virtual network device."""
import os
import json
import socket
import struct
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


QEMU = os.environ.get('MIBAND_BEST1700_QEMU')
BASE = 0x4ff00000
MAGIC = 0x4d424e54


@unittest.skipUnless(QEMU, 'requires native QEMU with the network bridge')
class MiBandNetworkTests(unittest.TestCase):
    def machine(self, **kwargs):
        machine_name = kwargs.pop('machine_name', 'miband-best1700')
        q = QtestMachine(QEMU, machine_options=',netdev=net0' + kwargs.pop('machine_options', ''),
                         serial_backends=['none'] * 5, machine_name=machine_name, **kwargs)
        self.addCleanup(q.close)
        return q

    def net_path(self, q):
        children = q.qmp('qom-list', {'path': '/machine/unattached'})
        entry = next(item for item in children
                     if item.get('type') == 'child<miband-net>')
        return '/machine/unattached/' + entry['name']

    def test_best1700_ap_reads_net_mmio(self):
        # LDR r0,net; LDR r1,[r0]; LDR r2,result; STR r1,[r2]; B .
        # Executed by the real AP address space, not QMP's system alias.
        code = bytes.fromhex('02480168024a1160fee700bf')
        image = struct.pack('<II', 0x203ffff0, 0x101c0009) + code + struct.pack('<II', BASE, 0x20001000)
        q = self.machine(accel='tcg', machine_options=',boot-pc=0x101c0009',
                         image_payload=image, extra_args=['-netdev', 'user,id=net0'])
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if q.read32(0x20001000) == MAGIC:
                return
            time.sleep(.01)
        self.fail('AP did not read the network device magic')

    def test_guest_network_status_is_exposed_without_stopping_guest(self):
        q = self.machine(accel='tcg', extra_args=['-S', '-netdev', 'user,id=net0'])
        stats = json.loads(q.qmp('qom-get', {
            'path': self.net_path(q), 'property': 'net-stats'}))
        self.assertEqual(stats['guest_state'], 0)
        self.assertEqual(stats['guest_error'], 0)
        self.assertFalse(stats['ready'])

    def test_rx_ack_drains_queued_burst(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        q = self.machine(extra_args=['-netdev', f'socket,id=net0,listen=127.0.0.1:{port}'])
        deadline = time.monotonic() + 5
        while True:
            peer = socket.socket()
            try:
                peer.connect(('127.0.0.1', port))
                break
            except ConnectionRefusedError:
                peer.close()
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.01)
        self.addCleanup(peer.close)
        q.qmp('cont')
        for i in range(12):
            frame = bytes.fromhex('52540a00020f52550a0002020800') + bytes([i]) + bytes(45)
            peer.sendall(struct.pack('!I', len(frame)) + frame)
        deadline = time.monotonic() + 5
        while q.read32(BASE + 0x30) < 8 and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertEqual(q.read32(BASE + 0x30), 8, 'the receive FIFO must first become full')
        seen = []
        deadline = time.monotonic() + 5
        while len(seen) < 12 and time.monotonic() < deadline:
            if q.read32(BASE + 8) & 4:
                seen.append(q.read32(BASE + 0x800) & 0xff)
                q.write32(BASE + 0x1c, 1)
            else:
                time.sleep(.01)
        self.assertEqual(seen, list(range(12)))


if __name__ == '__main__':
    unittest.main()
