"""Exercise HIFI4 guest-memory queues with an independent synthetic AP peer.

All generated image/profile fixtures and process logs live in temporary dirs.
This verifies the transport contract; firmware boot remains a separate check.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


ROOT = Path(__file__).resolve().parents[1]
SET, CLEAR = 0x40000090, 0x40000094
AON = 0x40080000
AP_CFG, AP_CHAN, AP_RECORD = 0x20010000, 0x20010100, 0x20010200
AP_RING = 0x20018000
BUFFERS = (0x20030000, 0x20040000)


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Hifi4EndpointQtest(unittest.TestCase):
    def setUp(self):
        self.profile = {'ipc': {'audio_endpoint': {
            'schema': 'bes.hifi4.endpoint.v1',
            'mailbox_ap': 0x20a80000, 'mailbox_dsp': 0x20a80004,
            'peer_config': 0x20a80100, 'peer_channels': 0x20a80200,
            'peer_records': 0x20a80400, 'resource_table': 0x20a81000,
            'resource_size': 0x1a0, 'vring': 0x20a82000, 'vring_num': 8,
            'buffer_size': 2048, 'channel_id': 3, 'version': 0x01010402,
            'features': 0x40000007, 'payload_offset': 64,
            'payload_size': 1024, 'payload_sha256': '0' * 64,
            'image_config_offset': 0x100, 'image_resource_offset': 0x200,
            'expected_reset_vector': 0x00880100, 'notify_mask': 0xffffff00,
            'start_token': 0xffffff11, 'reset_token': 0xffffff12,
            'panic_token': 0xffffff10, 'timeout_ms': 20000,
            'service_name': 'pcm0c', 'service_address': 1024,
            'request_hex': '0100140010000000000000000000000000000000',
            'response_hex': '0110140010000000000020005100000000000000',
        }}, 'images': {'audio': {
            'memory': {'dtcm_base': 0x20a80000, 'dtcm_size': 0x80000,
                       'itcm_size': 0x80000}, 'sha256': '0' * 64,
        }}}
        if getattr(self, 'fs_enabled', False):
            self.profile['ipc']['audio_endpoint']['rpmsgfs'] = {
                'mode': getattr(self, 'fs_mode', 'unavailable'), 'service_address': 1025}
            if getattr(self, 'fs_mode', None) == 'empty':
                self.profile['ipc']['audio_endpoint']['rpmsgfs']['root'] = '/proc'
        self.c = self.profile['ipc']['audio_endpoint']
        self.c.update(image_config_offset=0x100, image_resource_offset=0x200,
                      payload_size=0x400, expected_reset_vector=0x00880100)
        payload = bytearray(self.c['payload_size'])
        struct.pack_into('<I', payload, 0x10, self.c['expected_reset_vector'])
        struct.pack_into('<4I', payload, 0x14,
                         0x00880000, 0x00900000, 0x20880000, 0x20900000)
        cfg = self.c['image_config_offset']
        struct.pack_into('<II', payload, cfg + 4,
                         self.c['mailbox_ap'], self.c['mailbox_dsp'])
        struct.pack_into('<I', payload, cfg + 0x38, 0x00200002)
        struct.pack_into('<II', payload, cfg + 0x3c,
                         self.c['peer_channels'] - 0x200000,
                         self.c['peer_records'] - 0x200000)
        struct.pack_into('<II', payload, cfg + 0x50,
                         self.c['resource_table'] - 0x200000,
                         self.c['resource_table'] - 0x200000 + self.c['resource_size'])
        for offset, channel in ((0x200, self.c['channel_id']), (0x2d0, 8)):
            struct.pack_into('<II', payload, offset, 1, 1)
            struct.pack_into('<II', payload, offset + 0x94,
                             channel, self.c['version'])
        self.c['payload_sha256'] = hashlib.sha256(payload).hexdigest()
        temp = tempfile.TemporaryDirectory(prefix='hifi4-contract-')
        self.addCleanup(temp.cleanup)
        directory = Path(temp.name)
        image = directory / 'audio.bin'
        image.write_bytes(payload)
        config = directory / 'profile.json'
        config.write_text(json.dumps(self.profile))
        self.q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
            machine_options=f',audio-image={image},audio-endpoint-profile={config}')
        self.addCleanup(self.q.close)
        self.q.command('irq_intercept_in /machine/core[0]')
        self.avail = [0, 0]
        self.used = [0, 0]

    def status(self):
        return json.loads(self.q.qmp('qom-get',
            {'path': '/machine', 'property': 'audio-status'}))

    def start(self, vector=None):
        q = self.q
        q.write32(AON + 0x1a4, 4)
        q.write32(AON + 0x160, 1)
        q.write32(AON + 0x164,
                  self.c['expected_reset_vector'] if vector is None else vector)
        q.write32(AON + 0xcc, 1 << 10)
        q.write32(AON + 0x160, 0)
        q.write32(AON + 0x1a4, 0)
        q.step(1_000_000)

    def ack(self):
        self.q.write32(CLEAR, 0x200)
        self.q.write32(SET, 1)
        self.q.step(1_000_000)

    def notify(self, token):
        q = self.q
        q.write32(self.c['mailbox_ap'], AP_CFG)
        q.write32(AP_CFG + 0x3c, AP_CHAN)
        q.write32(AP_CHAN + 4, AP_RECORD)
        q.write(AP_RECORD, struct.pack('<5I', 0, 0, token, 0, 1))
        q.write32(SET, 8)
        q.step(1_000_000)
        self.assertTrue(q.read32(SET) & 0x40)
        self.assertTrue(q.irq(79))
        q.write32(CLEAR, 0x40)

    def ring(self, index):
        return self.c['vring'] if index == 0 else AP_RING

    def add_buffer(self, ring, message=None, *, address=None):
        q = self.q
        count, base = self.c['vring_num'], self.ring(ring)
        index = self.avail[ring] % count
        data = BUFFERS[ring] + index * self.c['buffer_size']
        if message is not None:
            q.write(data, message)
        q.write(base + index * 16, struct.pack('<QIHH',
            data if address is None else address,
            self.c['buffer_size'] if message is None else len(message),
            2 if ring == 0 else 0, 0))
        q.write(base + count * 16 + 4 + index * 2, struct.pack('<H', index))
        self.avail[ring] += 1
        q.write(base + count * 16 + 2, struct.pack('<H', self.avail[ring]))

    def take(self):
        q = self.q
        count, base = self.c['vring_num'], self.ring(0)
        used = base + ((count * 16 + (count + 3) * 2 + 7) & ~7)
        total = struct.unpack('<H', q.read(used + 2, 2))[0]
        self.assertGreater(total, self.used[0])
        index, length = struct.unpack('<II', q.read(
            used + 4 + 8 * (self.used[0] % count), 8))
        self.used[0] += 1
        raw = q.read(BUFFERS[0] + index * self.c['buffer_size'], length)
        src, dst, _, length, _ = struct.unpack_from('<IIIHH', raw)
        self.ack()
        return src, dst, raw[16:16 + length]

    def deliver(self, src, dst, payload):
        self.add_buffer(1, struct.pack('<IIIHH', src, dst, 0, len(payload), 0) + payload)
        self.notify(self.c['notify_mask'] | 1)

    def transport(self):
        self.start()
        self.assertEqual(self.q.read32(self.c['mailbox_dsp']), self.c['peer_config'])
        self.assertEqual(self.q.read32(self.c['peer_records'] + 8), self.c['start_token'])
        self.assertTrue(self.q.irq(76))
        self.ack()
        self.notify(self.c['start_token'])
        rsc = self.c['resource_table']
        self.q.write(rsc + 0x78, struct.pack('<5I', AP_RING, 8,
                                           self.c['vring_num'], 0xffffffff, 0))
        self.q.write32(rsc + 0x8c, self.c['buffer_size'])
        for _ in range(self.c['vring_num']):
            self.add_buffer(0)
        self.q.write(rsc + 0x60, b'\x07')
        self.q.step(1_000_000)

    def bind(self):
        self.transport()
        src, dst, payload = self.take()
        self.assertEqual((src, dst), (self.c['service_address'], 0x35))
        self.assertEqual(payload, self.c['service_name'].encode().ljust(32, b'\0') +
                         struct.pack('<II', self.c['service_address'], 0))
        ack = self.c['service_name'].encode().ljust(32, b'\0') + struct.pack('<II', 0x401, 2)
        self.deliver(0x401, 0x35, ack)
        self.q.step(1_000_000)
        src, dst, payload = self.take()
        self.assertEqual((src, dst), (self.c['service_address'], 0x401))
        self.assertEqual(payload, bytes.fromhex(self.c['request_hex']))
        self.assertFalse(self.status()['ready'])

    def test_ready_requires_guest_ns_ack_and_capability_response(self):
        self.bind()
        self.deliver(0x401, self.c['service_address'], bytes.fromhex(self.c['response_hex']))
        status = self.status()
        self.assertEqual(status['state'], 'service-ready')
        self.assertTrue(status['ready'])
        self.assertEqual(status['rpmsg_tx'], 2)
        self.assertEqual(status['rpmsg_rx'], 2)

    def test_wrong_service_response_fails_without_fabricating_ready(self):
        self.bind()
        response = bytearray.fromhex(self.c['response_hex'])
        response[1] = 0
        self.deliver(0x401, self.c['service_address'], response)
        self.assertEqual(self.status()['state'], 'failed')
        self.assertFalse(self.status()['ready'])

    def test_reserved_ns_ack_source_does_not_bind_service(self):
        self.transport()
        self.take()
        ack = self.c['service_name'].encode().ljust(32, b'\0') + struct.pack('<II', 0x401, 2)
        self.deliver(0x35, 0x35, ack)
        status = self.status()
        self.assertFalse(status['registered'])
        self.assertFalse(status['ready'])
        self.assertEqual(status['rpmsg_tx'], 1)
        self.assertEqual(status['unsupported'], 1)

    def test_timeout_is_failure_without_a_guest_handshake(self):
        self.start()
        self.q.step(self.c['timeout_ms'] * 1_000_000)
        self.assertEqual(self.status()['state'], 'failed')
        self.assertFalse(self.status()['ready'])

    def test_reset_clears_published_config_and_can_reopen(self):
        self.transport()
        self.q.write32(AON + 0xc8, 1 << 10)
        self.assertEqual(self.q.read32(self.c['mailbox_dsp']), 0)
        self.assertEqual(self.q.read32(SET) & 0x249, 0)
        self.assertFalse(self.status()['ready'])
        self.start()
        self.assertEqual(self.q.read32(self.c['mailbox_dsp']), self.c['peer_config'])
        self.assertEqual(self.q.read32(self.c['peer_records'] + 8), self.c['start_token'])

    def test_reset_vector_mismatch_refuses_publication(self):
        self.start(self.c['expected_reset_vector'] + 4)
        self.assertEqual(self.q.read32(self.c['mailbox_dsp']), 0)
        self.assertEqual(self.status()['state'], 'failed')

    def test_runstall_preserves_state_and_suspends_timeout(self):
        self.start()
        self.q.write32(AON + 0x160, 1)
        self.assertTrue(self.status()['paused'])
        self.q.step((self.c['timeout_ms'] + 1) * 1_000_000)
        self.assertEqual(self.status()['state'], 'ipc-open')
        self.q.write32(AON + 0x160, 0)
        self.assertFalse(self.status()['paused'])
        self.q.step(1_000_000)
        self.assertEqual(self.status()['state'], 'ipc-open')

    def test_descriptor_cannot_write_to_mmio(self):
        self.start()
        self.ack()
        rsc = self.c['resource_table']
        self.q.write(rsc + 0x78, struct.pack('<5I', AP_RING, 8,
                                           self.c['vring_num'], 0xffffffff, 0))
        self.add_buffer(0, address=SET)
        self.q.write(rsc + 0x60, b'\x07')
        self.q.step(1_000_000)
        self.assertEqual(self.status()['state'], 'failed')
        self.assertFalse(self.status()['ready'])


class UnavailableFilesystemQtest(Hifi4EndpointQtest):
    fs_enabled = True

    def fs_bind(self, src=0x450):
        self.transport()
        self.take()  # Original PCM name-service announcement.
        ns = b'rpmsgfs-synthetic'.ljust(32, b'\0') + struct.pack('<II', src, 0)
        self.deliver(src, 0x35, ns)
        origin, dest, ack = self.take()
        self.assertEqual((origin, dest), (1025, 0x35))
        self.assertEqual(ack, ns[:32] + struct.pack('<II', 1025, 2))
        return src

    @staticmethod
    def fs_request(command=20):
        return struct.pack('<IiQ', command, -6, 0x11223344aabbccdd) + bytes(88) + b'/proc/irqs\0'

    def test_fs_unavailable_returns_error_and_preserves_cookie_and_layout(self):
        src = self.fs_bind()
        request = self.fs_request()
        self.deliver(src, 1025, request)
        origin, dest, reply = self.take()
        self.assertEqual((origin, dest), (1025, src))
        self.assertEqual(reply, request[:4] + struct.pack('<i', -38) + request[8:])
        status = self.status()
        self.assertEqual(status['filesystem_mode'], 'unavailable')
        self.assertEqual(status['filesystem_bound'], 1)
        self.assertEqual(status['filesystem_rejected'], 1)
        self.assertFalse(status['ready'])  # Filesystem failure is not PCM success.

    def test_fs_replies_wait_for_rx_buffers_without_loss_or_duplicate(self):
        src = self.fs_bind()
        for _ in range(8):
            self.deliver(src, 1025, self.fs_request())
        self.assertEqual(self.status()['pending_replies'], 2)
        for _ in range(6):
            self.assertEqual(struct.unpack_from('<i', self.take()[2], 4)[0], -38)
        for _ in range(2):
            self.add_buffer(0)
        self.q.step(1_000_000)
        for _ in range(2):
            self.assertEqual(struct.unpack_from('<i', self.take()[2], 4)[0], -38)
        self.assertEqual(self.status()['pending_replies'], 0)
        self.assertEqual(self.status()['filesystem_rejected'], 8)

    def test_fs_unknown_source_or_malformed_request_is_not_answered(self):
        src = self.fs_bind()
        self.deliver(src + 1, 1025, self.fs_request())
        self.deliver(src, 1025, bytes(15))
        self.deliver(src, 1025, self.fs_request(27))
        self.assertEqual(self.status()['filesystem_rejected'], 0)
        self.assertEqual(self.status()['unsupported'], 3)

    def test_fs_destroy_unbinds_client_and_reset_clears_queue(self):
        src = self.fs_bind()
        ns = b'rpmsgfs-synthetic'.ljust(32, b'\0') + struct.pack('<II', src, 1)
        self.deliver(src, 0x35, ns)
        self.assertEqual(self.status()['filesystem_bound'], 0)
        self.deliver(src, 1025, self.fs_request())
        self.assertEqual(self.status()['filesystem_rejected'], 0)
        self.q.write32(AON + 0xc8, 1 << 10)
        self.assertEqual(self.status()['pending_replies'], 0)
        self.assertEqual(self.status()['filesystem_bound'], 0)


class EmptyFilesystemQtest(Hifi4EndpointQtest):
    fs_enabled = True
    fs_mode = 'empty'
    fs_bind = UnavailableFilesystemQtest.fs_bind

    def test_only_declared_root_has_real_directory_metadata(self):
        src = self.fs_bind()
        for path in (b'/proc\0', b'/proc/\0'):
            request = struct.pack('<IiQ', 20, -6, 0x12345678) + bytes(88) + path
            self.deliver(src, 1025, request)
            origin, dest, reply = self.take()
            self.assertEqual((origin, dest), (1025, src))
            self.assertEqual(reply[:16], struct.pack('<IiQ', 20, 0, 0x12345678))
            self.assertEqual(struct.unpack_from('<I', reply, 20)[0], 0o40555)
            self.assertEqual(struct.unpack_from('<H', reply, 102)[0], 2)
            self.assertEqual(reply[104:], path)
        self.assertEqual(self.status()['filesystem_root_stats'], 2)
        self.assertEqual(self.status()['filesystem_mode'], 'empty')

    def test_empty_root_does_not_invent_diagnostic_files_or_open_handles(self):
        src = self.fs_bind()
        for command, path in ((20, b'/proc/irqs\0'), (20, b'/other\0'),
                              (20, b'/proc/../proc\0'), (20, b'/proc'),
                              (1, b'/proc/irqs\0')):
            body = bytes(88) if command == 20 else struct.pack('<ii', 0, 0)
            request = struct.pack('<IiQ', command, -6, 0x12345678) + body + path
            self.deliver(src, 1025, request)
            reply = self.take()[2]
            self.assertEqual(reply, request[:4] + struct.pack('<i', -2) + request[8:])
        self.assertEqual(self.status()['filesystem_root_stats'], 0)
        self.assertEqual(self.status()['filesystem_rejected'], 5)


if __name__ == '__main__':
    unittest.main()
