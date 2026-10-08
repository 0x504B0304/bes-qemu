"""Exercise SYS/BTH doorbells without transporting or synthesizing guest data.

Each observer uses its own QEMU process because qtest can intercept one
ARMv7M device at a time. All memory fixtures are synthetic and temporary.
"""
from contextlib import contextmanager
import os
import struct
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


SIDES = {
    'sys': dict(core=0, set=0x40000098, clear=0x4000009c,
                mask_set=0x400001f4, mask_clear=0x400001f8,
                tx=0x40000224, rx=0x400000ec,
                rx_irq=(96, 97), done_irq=(94, 95)),
    'bth': dict(core=2, set=0x50000134, clear=0x50000138,
                mask_set=0x50000168, mask_clear=0x5000016c,
                tx=0x50000170, rx=0x50000174,
                rx_irq=(73, 74), done_irq=(65, 66)),
}


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class SysBthIpcQtest(unittest.TestCase):
    @contextmanager
    def observer(self, side):
        machine = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                                machine_options=',bth-real-boot=on')
        try:
            local = SIDES[side]
            peer = SIDES['bth' if side == 'sys' else 'sys']
            machine.command(f'irq_intercept_in /machine/core[{local["core"]}]')
            yield machine, local, peer
        finally:
            machine.close()

    def assert_irqs(self, q, local, *, rx=(False, False), done=(False, False)):
        for number, expected in zip(local['rx_irq'], rx):
            self.assertEqual(q.irq(number), expected, f'RX IRQ {number}')
        for number, expected in zip(local['done_irq'], done):
            self.assertEqual(q.irq(number), expected, f'DONE IRQ {number}')

    def assert_pending(self, q, side, value):
        self.assertEqual(q.read32(side['set']), value)
        self.assertEqual(q.read32(side['clear']), value)

    def assert_mask(self, q, side, value):
        self.assertEqual(q.read32(side['mask_set']), value)
        self.assertEqual(q.read32(side['mask_clear']), value)

    def test_masks_gate_pending_rx_and_done_and_retrigger(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local, peer):
                q.write32(peer['set'], 1 << 16)
                self.assertEqual(q.read32(local['rx']), 1)
                self.assert_irqs(q, local)
                q.write32(local['mask_set'], 1)
                self.assertEqual(q.read32(local['rx']), 0x00010001)
                self.assert_mask(q, local, 1)
                self.assert_irqs(q, local, rx=(True, False))
                q.write32(local['mask_clear'], 1)
                self.assertEqual(q.read32(local['rx']), 1)
                self.assert_irqs(q, local)
                q.write32(local['mask_set'], 1)
                self.assert_irqs(q, local, rx=(True, False))
                q.write32(peer['clear'], 1 << 16)
                self.assertEqual(q.read32(local['rx']), 0)
                self.assert_irqs(q, local)

                q.write32(local['set'], 2 << 16)
                q.write32(local['clear'], 2 << 16)
                self.assertEqual(q.read32(local['tx']), 2)
                self.assert_irqs(q, local)
                q.write32(local['mask_set'], 2 << 16)
                self.assertEqual(q.read32(local['tx']), 0x00020002)
                self.assert_mask(q, local, 0x00020001)
                self.assert_irqs(q, local, done=(False, True))
                q.write32(local['mask_clear'], 2 << 16)
                self.assertEqual(q.read32(local['tx']), 2)
                self.assert_irqs(q, local)
                q.write32(local['mask_set'], 2 << 16)
                self.assert_irqs(q, local, done=(False, True))
                q.write32(local['clear'], 2)
                self.assert_irqs(q, local)

    def test_clear_asserted_indication_produces_done_once_and_coalesces(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local, peer):
                q.write32(local['mask_set'], 0x00030003)
                q.write32(peer['mask_set'], 0x00030003)
                # An empty clear must never invent a completion.
                q.write32(local['clear'], 1 << 16)
                self.assert_pending(q, local, 0)
                self.assert_irqs(q, local)
                for _ in range(3):
                    q.write32(local['set'], 1 << 16)
                self.assert_pending(q, local, 1 << 16)
                self.assertEqual(q.read32(peer['rx']), 0x00010001)
                q.write32(local['clear'], 1 << 16)
                self.assert_pending(q, local, 1)
                self.assertEqual(q.read32(peer['rx']), 0)
                self.assert_irqs(q, local, done=(True, False))
                q.write32(local['clear'], 1)
                q.write32(local['clear'], 1 << 16)
                self.assert_pending(q, local, 0)
                self.assert_irqs(q, local)
                # Reasserting after acknowledgement is a new exchange.
                q.write32(local['set'], 1 << 16)
                q.write32(local['clear'], 1 << 16)
                self.assert_pending(q, local, 1)
                self.assert_irqs(q, local, done=(True, False))

    def test_full_duplex_channels_preserve_each_others_pending_state(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local, peer):
                for registers in (local, peer):
                    q.write32(registers['mask_set'], 0x00030003)
                    q.write32(registers['set'], 3 << 16)
                self.assert_irqs(q, local, rx=(True, True))
                q.write32(local['clear'], 1 << 16)
                self.assert_pending(q, local, (2 << 16) | 1)
                self.assertEqual(q.read32(peer['rx']), 0x00020002)
                self.assert_irqs(q, local, rx=(True, True), done=(True, False))
                q.write32(peer['clear'], 2 << 16)
                self.assertEqual(q.read32(local['rx']), 0x00010001)
                self.assertEqual(q.read32(peer['tx']), 0x00020002)
                self.assert_irqs(q, local, rx=(True, False), done=(True, False))
                q.write32(local['clear'], 1)
                self.assert_irqs(q, local, rx=(True, False))
                q.write32(peer['clear'], 1 << 16)
                q.write32(local['clear'], 2 << 16)
                self.assert_irqs(q, local, done=(False, True))
                q.write32(local['clear'], 2)
                q.write32(peer['clear'], 3)
                self.assert_pending(q, local, 0)
                self.assert_pending(q, peer, 0)
                self.assert_irqs(q, local)

    def test_status_is_read_only_and_masks_do_not_destroy_raw_state(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local, peer):
                q.write32(local['mask_set'], 0x00020001)
                q.write32(peer['set'], 3 << 16)
                q.write32(local['set'], 3)
                expected = {local['rx']: 0x00010003, local['tx']: 0x00020003}
                for address, value in expected.items():
                    self.assertEqual(q.read32(address), value)
                    for write_value in (0, 0xffffffff):
                        q.write32(address, write_value)
                        self.assertEqual(q.read32(address), value)
                    q.command(f'writeb {address:#x} 0')
                    q.command(f'writew {address + 2:#x} 0xffff')
                    self.assertEqual(q.read32(address), value)
                self.assert_irqs(q, local, rx=(True, False), done=(False, True))
                q.write32(local['mask_clear'], 0xffffffff)
                self.assertEqual(q.read32(local['rx']), 3)
                self.assertEqual(q.read32(local['tx']), 3)
                self.assert_irqs(q, local)

    def test_byte_and_halfword_lanes_update_only_addressed_bits(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local, peer):
                q.command(f'writew {peer["set"] + 2:#x} 3')
                self.assert_pending(q, peer, 3 << 16)
                q.command(f'writeb {local["mask_set"]:#x} 1')
                self.assert_irqs(q, local, rx=(True, False))
                q.command(f'writeb {local["mask_set"] + 2:#x} 2')
                self.assert_mask(q, local, 0x00020001)
                q.command(f'writeb {local["set"]:#x} 3')
                q.command(f'writeb {local["set"] + 1:#x} 0x80')
                self.assert_pending(q, local, 0x8003)
                self.assert_irqs(q, local, rx=(True, False), done=(False, True))
                q.command(f'writeb {local["clear"] + 1:#x} 0x80')
                self.assert_pending(q, local, 3)
                q.command(f'writeb {peer["clear"] + 2:#x} 1')
                self.assert_pending(q, peer, (2 << 16) | 1)
                self.assertEqual(q.read32(local['rx']), 2)
                self.assert_irqs(q, local, done=(False, True))
                q.command(f'writeb {local["mask_clear"] + 2:#x} 2')
                self.assert_mask(q, local, 1)
                self.assert_irqs(q, local)

    def test_high_lanes_never_alias_sensor_channel_irqs(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local, peer):
                q.write32(local['mask_set'], 0xffffffff)
                q.write32(peer['mask_set'], 0xffffffff)
                for lane in (2, 7, 8, 15):
                    bit = 1 << lane
                    with self.subTest(lane=lane):
                        q.write32(peer['set'], bit << 16)
                        self.assertEqual(q.read32(local['rx']), (bit << 16) | bit)
                        self.assert_irqs(q, local)
                        q.write32(local['set'], bit << 16)
                        q.write32(local['clear'], bit << 16)
                        self.assertEqual(q.read32(local['tx']), (bit << 16) | bit)
                        self.assert_irqs(q, local)
                        q.write32(peer['clear'], bit << 16)
                        q.write32(peer['clear'], bit)
                        q.write32(local['clear'], bit)
                        self.assert_pending(q, local, 0)
                        self.assert_pending(q, peer, 0)

    def test_system_reset_clears_signals_and_allows_new_registration(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local, peer):
                markers = {0x18007000: struct.pack('<4I', 0x11, 0x22, 0x33, 0x44),
                           0x23000400: b'unchanged-data!!'}
                for address, payload in markers.items():
                    q.write(address, payload)
                for registers in (local, peer):
                    q.write32(registers['mask_set'], 0x00030003)
                    q.write32(registers['set'], 0x00030003)
                self.assert_irqs(q, local, rx=(True, True), done=(True, True))
                q.qmp('system_reset')
                q.qmp('query-status')
                for registers in (local, peer):
                    self.assert_pending(q, registers, 0)
                    self.assert_mask(q, registers, 0)
                    self.assertEqual(q.read32(registers['rx']), 0)
                    self.assertEqual(q.read32(registers['tx']), 0)
                self.assert_irqs(q, local)
                q.write32(local['mask_set'], 1)
                q.write32(peer['mask_set'], 1 << 16)
                q.write32(peer['set'], 1 << 16)
                self.assert_irqs(q, local, rx=(True, False))
                q.write32(peer['clear'], 1 << 16)
                self.assert_irqs(q, local)
                self.assertEqual(q.read32(peer['tx']), 0x00010001)
                for address, payload in markers.items():
                    self.assertEqual(q.read(address, len(payload)), payload,
                                     'doorbell/reset must not synthesize or copy payload RAM')


if __name__ == '__main__':
    unittest.main()
