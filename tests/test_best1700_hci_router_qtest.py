"""External/native mailbox arbitration, without firmware or physical hardware."""
import json
import os
import queue
import socket
import struct
import threading
import time
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'), 'native QEMU required')
class HciRouterTests(unittest.TestCase):
    def setUp(self):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.listener.settimeout(5)
        self.peer = None
        self.ready = threading.Event()
        self.incoming = queue.Queue()
        self.errors = []
        self.closing = False

        def receive_exact(sock, size):
            data = b''
            while len(data) < size:
                chunk = sock.recv(size - len(data))
                if not chunk:
                    raise EOFError()
                data += chunk
            return data

        def server():
            try:
                self.peer, _ = self.listener.accept()
                self.peer.settimeout(5)
                greeting = b''
                while not greeting.endswith(b'\n'):
                    greeting += receive_exact(self.peer, 1)
                self.assertEqual(greeting, b'BES-HCI/2 ' + b'a'*32 + b' ' +
                                 b'b'*32 + b' ' + b'c'*64 + b'\n')
                self.peer.sendall(b'BES-HCI/2 OK\n')
                self.ready.set()
                while True:
                    header = receive_exact(self.peer, 4)
                    self.incoming.put(header + receive_exact(self.peer, header[3]))
            except (EOFError, OSError) as exc:
                if not self.closing:
                    self.errors.append(exc)
            except BaseException as exc:
                self.errors.append(exc)
                self.ready.set()

        self.thread = threading.Thread(target=server, daemon=True)
        self.thread.start()
        backend = ('host-bridge-v2:tcp://127.0.0.1:' + str(self.listener.getsockname()[1]) +
                   '|' + 'a'*32 + '|' + 'b'*32 + '|' + 'c'*64)
        self.q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
            machine_options=(',cp-real-boot=on,bth-real-boot=on,btc-real-boot=on'
                ',bt-mailbox=68004000:68018000:fc77:1001,bt-backend=' + backend),
            extra_args=['-smp', '4', '-S'])
        self.addCleanup(self.close)
        self.assertTrue(self.ready.wait(5), self.q.diagnostic('handshake'))
        self.assertEqual(self.errors, [])
        try:
            self.assertFalse(self.q.qmp('query-status')['running'])
        except Exception:
            print(self.q.diagnostic('initial status'))
            raise
        self.q.command('irq_intercept_in /machine/core[1]')
        self.q.write32(0x40000140, (1 << 8) | (1 << 12))
        self.q.write32(0x68004000, 0x68008000)
        self.q.write32(0x68004004, 0x68008100)
        self.q.write32(0x68008100, 0x68008200)

    def close(self):
        self.closing = True
        self.q.close()
        if self.peer:
            self.peer.close()
        self.listener.close()
        self.thread.join(6)

    def status(self):
        return json.loads(self.q.qmp('qom-get', {'path': '/machine', 'property': 'bt-status'}))

    def wait(self, fn):
        deadline = time.monotonic() + 3
        while not fn():
            if time.monotonic() > deadline:
                self.fail(self.q.diagnostic('condition timeout'))
            time.sleep(.01)

    def send(self, packet, next_node=0):
        self.q.write(0x68008200, struct.pack('<IIII', next_node, 0, len(packet), 0x68008300))
        self.q.write(0x68008300, packet)
        self.q.write32(0x40000140, 1 << 4)

    def test_external_response_does_not_ack_or_overwrite_native_btc(self):
        self.send(bytes.fromhex('01 03 0c 00'))
        self.assertEqual(self.incoming.get(timeout=3), bytes.fromhex('01 03 0c 00'))
        self.assertTrue(self.q.read32(0x40000140) & 1)  # CP TX completed
        self.assertFalse(self.q.read32(0x40000140) & 16)  # no native receive
        response = bytes.fromhex('04 0e 04 01 03 0c 00')
        self.peer.sendall(response)
        self.wait(lambda: self.status()['pending'])
        self.assertTrue(self.q.irq(104))
        self.assertEqual(self.q.read32(0x68004000), 0x68008000)  # actual RAM unchanged
        cp_view = self.q.qmp('human-monitor-command',
                            {'command-line': 'x /1wx 0x68004000', 'cpu-index': 1})
        self.assertIn('0x68018000', cp_view)
        self.assertEqual(self.q.read(0x68018010, len(response)), response)
        self.q.write32(0x68018010, 0xdeadbeef)  # read-only receive aperture
        self.assertEqual(self.q.read(0x68018010, len(response)), response)

        # Original BTC publishes a response while the external one is visible.
        self.q.write32(0x6e030050, 1 << 18)
        self.q.write32(0x6e030054, 1 << 18)
        self.assertFalse(self.status()['pending'])
        self.assertTrue(self.q.irq(104))  # native response still pending
        self.assertEqual(self.q.read32(0x6e030050) & (1 << 16), 0)
        cp_view = self.q.qmp('human-monitor-command',
                            {'command-line': 'x /1wx 0x68004000', 'cpu-index': 1})
        self.assertIn('0x68008000', cp_view)
        self.q.write32(0x6e030054, 1 << 18)
        self.assertFalse(self.q.irq(104))
        self.assertTrue(self.q.read32(0x6e030050) & (1 << 16))

    def test_native_command_and_control_lane_remain_owned_by_original_btc(self):
        self.send(bytes.fromhex('01 77 fc 00'))
        self.assertEqual(self.status()['tx'], 0)
        self.assertTrue(self.q.read32(0x40000140) & 16)
        self.assertFalse(self.q.read32(0x40000140) & 1)
        self.q.write32(0x40000140, 1 << 5)
        self.assertTrue(self.q.read32(0x40000140) & (1 << 5))
        self.assertFalse(self.q.read32(0x40000140) & 2)
        self.assertTrue(self.incoming.empty())

    def test_malformed_chain_stops_without_sending_or_acknowledging(self):
        self.send(bytes.fromhex('01 03 0c 00'), 0x68008200)
        self.assertEqual(self.status()['state'], 'failed')
        self.assertEqual(self.status()['tx'], 0)
        self.assertFalse(self.q.read32(0x40000140) & 1)
        self.assertFalse(self.q.qmp('query-status')['running'])

    def test_receive_backlog_is_bounded_and_reset_closes_session(self):
        packet = bytes.fromhex('04 ff 01 00')
        self.peer.sendall(packet * 34)
        self.wait(lambda: self.status()['state'] == 'failed')
        self.assertEqual(self.status()['queued'], 32)
        self.q.qmp('system_reset')
        self.q.process.wait(timeout=5)
        self.assertEqual(self.q.process.returncode, 0)


if __name__ == '__main__':
    unittest.main()
