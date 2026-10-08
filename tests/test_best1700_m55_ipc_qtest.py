"""Check the shared SYS CMU interrupt latches for M55C0 and M55C1.

The two processors share SET/CLEAR words. Receive acknowledgement clears
IND and explicitly sets the sender's DONE bit; clearing IND alone must not
invent a completion. Every observer uses an independent QEMU process.
"""
from contextlib import contextmanager
import os
import struct
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


SET, CLEAR = 0x40000090, 0x40000094
M55_MASK = 0x00fff000
HIFI_BITS = 0x249
SIDES = {
    'ap': dict(core=0, incoming=(21, 22, 23), outgoing=(15, 16, 17),
               done=(18, 19, 20), peer_done=(12, 13, 14)),
    'cp': dict(core=1, incoming=(15, 16, 17), outgoing=(21, 22, 23),
               done=(12, 13, 14), peer_done=(18, 19, 20)),
}


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class M55IpcQtest(unittest.TestCase):
    @contextmanager
    def observer(self, side):
        q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                         machine_options=',cp-real-boot=on')
        try:
            local = SIDES[side]
            q.command(f'irq_intercept_in /machine/core[{local["core"]}]')
            yield q, local
        finally:
            q.close()

    def assert_pending(self, q, value):
        self.assertEqual(q.read32(SET), value)
        self.assertEqual(q.read32(CLEAR), value)

    def assert_irqs(self, q, local, pending):
        for lane in range(3):
            self.assertEqual(q.irq(88 + lane), bool(pending & (1 << local['incoming'][lane])),
                             f'RX lane {lane} on CPU{local["core"]}')
            self.assertEqual(q.irq(91 + lane), bool(pending & (1 << local['done'][lane])),
                             f'DONE lane {lane} on CPU{local["core"]}')

    def test_receive_acknowledgement_requires_explicit_done_set(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local):
                for lane in range(3):
                    incoming = 1 << local['incoming'][lane]
                    outgoing = 1 << local['outgoing'][lane]
                    done = 1 << local['done'][lane]
                    peer_done = 1 << local['peer_done'][lane]
                    q.write32(CLEAR, incoming | outgoing)
                    self.assert_pending(q, 0)
                    q.write32(SET, incoming)
                    self.assert_irqs(q, local, incoming)
                    q.write32(CLEAR, incoming)
                    self.assert_pending(q, 0)
                    self.assert_irqs(q, local, 0)
                    q.write32(SET, peer_done)
                    self.assert_pending(q, peer_done)
                    self.assert_irqs(q, local, peer_done)
                    q.write32(CLEAR, peer_done)

                    q.write32(SET, outgoing)
                    q.write32(CLEAR, outgoing)
                    self.assert_pending(q, 0)
                    self.assert_irqs(q, local, 0)
                    q.write32(SET, done)
                    self.assert_pending(q, done)
                    self.assert_irqs(q, local, done)
                    q.write32(CLEAR, done)
                    self.assert_irqs(q, local, 0)

    def test_notifications_coalesce_and_channels_clear_independently(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local):
                first, second = (1 << bit for bit in local['incoming'][:2])
                for _ in range(3):
                    q.write32(SET, first)
                q.write32(SET, second)
                self.assert_pending(q, first | second)
                self.assert_irqs(q, local, first | second)
                q.write32(CLEAR, first)
                self.assert_pending(q, second)
                self.assert_irqs(q, local, second)
                q.write32(SET, 0)
                q.write32(CLEAR, 0)
                self.assert_pending(q, second)
                q.write32(CLEAR, second)
                q.write32(CLEAR, first | second)
                self.assert_pending(q, 0)
                self.assert_irqs(q, local, 0)
                q.write32(SET, first)
                self.assert_irqs(q, local, first)

    def test_full_duplex_indications_and_completions_do_not_clobber_each_other(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local):
                incoming = [1 << bit for bit in local['incoming']]
                outgoing = [1 << bit for bit in local['outgoing']]
                pending = sum(incoming + outgoing)
                q.write32(SET, pending)
                self.assert_pending(q, pending)
                self.assert_irqs(q, local, pending)
                q.write32(CLEAR, incoming[0])
                pending &= ~incoming[0]
                q.write32(SET, 1 << local['peer_done'][0])
                pending |= 1 << local['peer_done'][0]
                self.assert_pending(q, pending)
                self.assert_irqs(q, local, pending)
                q.write32(CLEAR, outgoing[1])
                pending &= ~outgoing[1]
                q.write32(SET, 1 << local['done'][1])
                pending |= 1 << local['done'][1]
                self.assert_pending(q, pending)
                self.assert_irqs(q, local, pending)
                q.write32(CLEAR, 1 << local['done'][1])
                pending &= ~(1 << local['done'][1])
                self.assert_pending(q, pending)
                self.assert_irqs(q, local, pending)
                q.write32(CLEAR, M55_MASK)
                self.assert_pending(q, 0)
                self.assert_irqs(q, local, 0)

    def test_byte_and_halfword_commands_preserve_other_lanes(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local):
                q.command(f'writeb {SET + 1:#x} 0x80')
                pending = 1 << 15
                self.assert_pending(q, pending)
                self.assert_irqs(q, local, pending)
                q.command(f'writeb {SET + 2:#x} 0x65')
                pending |= 0x65 << 16
                self.assert_pending(q, pending)
                self.assert_irqs(q, local, pending)
                q.command(f'writeb {CLEAR + 2:#x} 0x20')
                pending &= ~(0x20 << 16)
                self.assert_pending(q, pending)
                self.assert_irqs(q, local, pending)
                q.command(f'writeb {CLEAR + 1:#x} 0x80')
                pending &= ~(1 << 15)
                self.assert_pending(q, pending)
                q.command(f'writew {SET:#x} 0x3000')
                pending |= 0x3000
                self.assert_pending(q, pending)
                self.assert_irqs(q, local, pending)
                q.command(f'writew {CLEAR + 2:#x} 0x00ff')
                pending &= ~0x00ff0000
                self.assert_pending(q, pending)
                self.assert_irqs(q, local, pending)
                q.command(f'writew {CLEAR:#x} 0xf000')
                self.assert_pending(q, 0)
                self.assert_irqs(q, local, 0)

    def test_third_channel_has_independent_receive_and_done_irqs(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local):
                for bit in (14, 17, 20, 23):
                    with self.subTest(third_channel_bit=bit):
                        q.write32(CLEAR, M55_MASK)
                        q.write32(SET, 1 << bit)
                        self.assert_pending(q, 1 << bit)
                        self.assert_irqs(q, local, 1 << bit)
                        q.write32(CLEAR, 1 << bit)
                        self.assert_pending(q, 0)

    def test_m55_commands_preserve_hifi_bits_and_low_byte_commands_preserve_m55(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local):
                m55 = (1 << local['incoming'][0]) | (1 << local['done'][1])
                q.write32(SET, HIFI_BITS)
                q.write32(SET, m55)
                self.assert_pending(q, HIFI_BITS | m55)
                self.assert_irqs(q, local, m55)
                q.write32(CLEAR, M55_MASK)
                self.assert_pending(q, HIFI_BITS)
                q.write32(SET, m55)
                q.command(f'writeb {CLEAR:#x} 0x49')
                self.assert_pending(q, 0x200 | m55)
                q.command(f'writeb {CLEAR + 1:#x} 2')
                self.assert_pending(q, m55)
                self.assert_irqs(q, local, m55)
                q.write32(CLEAR, M55_MASK)
                self.assert_pending(q, 0)

    def test_system_reset_clears_latches_and_re_registration_does_not_touch_ram(self):
        for side in SIDES:
            with self.subTest(observer=side), self.observer(side) as (q, local):
                markers = {0x18007000: b'mailbox-payload!',
                           0x21c04000: struct.pack('<4I', 0x11, 0x22, 0x33, 0x44)}
                for address, payload in markers.items():
                    q.write(address, payload)
                q.write32(SET, M55_MASK | HIFI_BITS)
                self.assert_irqs(q, local, M55_MASK)
                q.qmp('system_reset')
                q.qmp('query-status')
                self.assert_pending(q, 0)
                self.assert_irqs(q, local, 0)
                pending = (1 << local['incoming'][0]) | (1 << local['done'][1])
                q.write32(SET, pending)
                self.assert_pending(q, pending)
                self.assert_irqs(q, local, pending)
                q.write32(CLEAR, pending)
                self.assert_irqs(q, local, 0)
                for address, payload in markers.items():
                    self.assertEqual(q.read(address, len(payload)), payload,
                                     'interrupt latches must not synthesize or copy payload data')


if __name__ == '__main__':
    unittest.main()
