"""DSI command transport tests using guest MMIO and board response fixtures."""

import os
import subprocess
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


DSI = 0x401D0000
RX_STATUS = DSI + 0x10
COMMAND = DSI + 0x14
TX_HEADER = DSI + 0x18
TX_WORD = DSI + 0x1C
TX_FINAL1 = DSI + 0x20
READ_ENABLE = 1 << 15
CMD_DONE = 1 << 0
BTA_DONE = 1 << 1
BTA_FAIL = 1 << 2
RX_DONE = 1 << 3
RX_ERROR = 1 << 4
TIMEOUT = 1 << 6
READ_DONE = CMD_DONE | BTA_DONE | RX_DONE
READ_FAILED = CMD_DONE | BTA_FAIL | TIMEOUT


class DsiTestCase(unittest.TestCase):
    def start_machine(self, options=''):
        self.machine = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                                    machine_options=options)
        self.addCleanup(self.machine.close)
        self.machine.command('irq_intercept_in /machine/core[0]')

    def submit(self, *, operation=4, wire_length=5):
        # LP packet commands use five wire bytes for a short packet and
        # payload length + seven for a long packet. Bit 4 is LP busy.
        self.machine.write32(COMMAND, (wire_length << 16) | 0xE100 | operation)

    def read_dcs(self, command):
        self.machine.write32(TX_HEADER, 0x06 | (command << 8))
        self.submit(operation=6)

    def max_return(self, count):
        self.machine.write32(TX_HEADER, 0x37 | (count << 8))
        self.submit()

    def events(self):
        return (self.machine.read32(RX_STATUS) >> 16) & 0xFF

    def clear(self, events=0xFF, *, mask=0):
        self.machine.write32(RX_STATUS, (events << 16) | (mask << 24))

    def pop_reply(self, expected, *, mask=0):
        q = self.machine
        peek = q.read32(RX_STATUS)
        self.assertEqual((peek >> 8) & 31, min(len(expected), 31))
        self.assertEqual(peek & 0xFF, expected[0])
        self.assertEqual(q.read32(RX_STATUS), peek,
                         'FIFO reads must only pop when RX_READ_ENABLE is set')
        q.write32(RX_STATUS, READ_ENABLE | (mask << 24))
        received = []
        for remaining in range(len(expected), 0, -1):
            value = q.read32(RX_STATUS)
            self.assertEqual((value >> 8) & 31, min(remaining, 31),
                             'count must describe the FIFO before this read pops')
            received.append(value & 0xFF)
        self.assertEqual(bytes(received), expected)
        self.assertEqual(q.read32(RX_STATUS) & 0x1FFF, 0)
        q.write32(RX_STATUS, mask << 24)


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700DsiQtest(DsiTestCase):
    def setUp(self):
        self.start_machine()

    def test_no_panel_read_reports_failure_with_mask_and_w1c_irq28(self):
        q = self.machine
        self.assertEqual(q.read32(RX_STATUS), 0)
        q.write32(TX_HEADER, 0x0406)
        q.write32(COMMAND, 0x5E116)
        self.assertEqual(q.read32(COMMAND) & (1 << 4), 0,
                         'completed command must release LP busy')
        self.assertEqual(self.events(), READ_FAILED)
        self.assertEqual(q.read32(RX_STATUS) & 0x1FFF, 0,
                         'an absent panel must not supply identity bytes')
        self.assertFalse(q.irq(28))
        self.clear(0, mask=CMD_DONE)
        self.assertTrue(q.irq(28), 'unmasking a pending event must assert IRQ28')
        self.clear(CMD_DONE, mask=CMD_DONE)
        self.assertEqual(self.events(), BTA_FAIL | TIMEOUT)
        self.assertFalse(q.irq(28))
        self.clear(0, mask=TIMEOUT)
        self.assertTrue(q.irq(28))
        self.clear(BTA_FAIL, mask=TIMEOUT)
        self.assertEqual(self.events(), TIMEOUT)
        self.assertTrue(q.irq(28), 'W1C of another event must retain timeout')
        self.clear(TIMEOUT, mask=TIMEOUT)
        self.assertEqual(self.events(), 0)
        self.assertFalse(q.irq(28))

    def test_short_and_long_write_packet_lengths(self):
        q = self.machine
        for header in (0x1105, 0x663A15):
            with self.subTest(short_header=hex(header)):
                self.clear()
                q.write32(TX_HEADER, header)
                self.submit()
                self.assertEqual(self.events(), CMD_DONE)
        # Exercise every final-byte port, including a full four-byte tail,
        # then packet lengths with one or more complete preceding words.
        for length in (1, 2, 3, 4, 5, 8, 11):
            with self.subTest(long_length=length):
                self.clear()
                q.write32(TX_HEADER, 0x39 | (length << 8))
                payload = bytes(range(0x20, 0x20 + length))
                full = (length - 1) // 4
                for index in range(full):
                    q.write32(TX_WORD, int.from_bytes(payload[index * 4:index * 4 + 4], 'little'))
                tail = payload[full * 4:]
                q.write32(TX_FINAL1 + (len(tail) - 1) * 4,
                          int.from_bytes(tail, 'little'))
                self.submit(wire_length=length + 7)
                self.assertEqual(self.events(), CMD_DONE)
                self.assertEqual(q.read32(COMMAND) & (1 << 4), 0)
        for length, supplied, wire_length in ((0, 0, 7), (3, 2, 10), (2, 2, 8)):
            with self.subTest(length=length, supplied=supplied, wire=wire_length):
                self.clear()
                q.write32(TX_HEADER, 0x39 | (length << 8))
                if supplied:
                    q.write32(TX_FINAL1 + (supplied - 1) * 4, 0x3412)
                self.submit(wire_length=wire_length)
                self.assertEqual(self.events(), CMD_DONE | RX_ERROR)


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700DsiPropertyTest(unittest.TestCase):
    def test_malformed_board_read_responses_reject_startup(self):
        for response in ('04:xyz', '04:123', '04:01;04:02'):
            with self.subTest(response=response):
                process = subprocess.run(
                    [os.environ['MIBAND_BEST1700_QEMU'], '-M',
                     'miband-best1700,panel-read-responses=' + response,
                     '-display', 'none', '-monitor', 'none', '-serial', 'none', '-S'],
                    capture_output=True, text=True, timeout=5)
                self.assertNotEqual(process.returncode, 0)
                self.assertIn('panel-read-responses requires unique CC:HEX', process.stderr)


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700DsiPanelQtest(DsiTestCase):
    def setUp(self):
        self.start_machine(',panel-read-responses=04:5c080c;0a:9c;0b:1234;0c:' +
                           bytes(range(32)).hex())

    def test_dcs_short_and_long_replies_have_exact_wire_framing(self):
        q = self.machine
        # Fixed protocol vectors include the LP escape byte, header ECC
        # and (for a long response) little-endian reflected CCITT CRC.
        cases = ((0x0A, 1, '87 21 9c 00 1e'),
                 (0x0B, 2, '87 22 12 34 20'),
                 (0x04, 3, '87 1c 03 00 16 5c 08 0c df 1b'))
        for command, count, wire in cases:
            with self.subTest(command=hex(command)):
                self.max_return(count)
                self.clear(mask=RX_DONE)
                self.read_dcs(command)
                self.assertEqual(self.events(), READ_DONE)
                self.assertTrue(q.irq(28))
                self.pop_reply(bytes.fromhex(wire), mask=RX_DONE)
                self.assertEqual(self.events(), READ_DONE,
                                 'draining the FIFO must not acknowledge interrupts')
                self.assertTrue(q.irq(28))
                self.clear(RX_DONE, mask=RX_DONE)
                self.assertFalse(q.irq(28))

    def test_fifo_count_saturates_at_31_and_preserves_complete_packet(self):
        self.max_return(32)
        self.clear()
        self.read_dcs(0x0C)
        self.assertEqual(self.events(), READ_DONE)
        expected = bytes.fromhex('87 1c 20 00 39') + bytes(range(32)) + bytes.fromhex('1b ac')
        self.pop_reply(expected)

    def test_unread_reply_is_preserved_on_second_bus_turnaround(self):
        self.read_dcs(0x0A)
        self.assertEqual(self.events(), READ_DONE)
        self.read_dcs(0x0A)
        self.assertEqual(self.events(), READ_DONE | BTA_FAIL | RX_ERROR)
        self.pop_reply(bytes.fromhex('87 21 9c 00 1e'))

    def test_unconfigured_read_fails_without_fabricated_response(self):
        q = self.machine
        self.read_dcs(0xFF)
        self.assertEqual(self.events(), READ_FAILED)
        self.assertEqual(q.read32(RX_STATUS) & 0x1FFF, 0)

    def test_reset_clears_reply_events_and_mask_but_keeps_board_endpoint(self):
        q = self.machine
        self.max_return(3)
        self.clear(mask=RX_DONE)
        self.read_dcs(0x04)
        self.assertEqual((q.read32(RX_STATUS) >> 8) & 31, 10)
        self.assertTrue(q.irq(28))
        q.qmp('system_reset')
        q.qmp('query-status')
        self.assertEqual(q.read32(RX_STATUS), 0)
        self.assertEqual(q.read32(COMMAND), 0)
        self.assertEqual(q.read32(TX_HEADER), 0)
        self.assertFalse(q.irq(28))
        # Reset restores the one-byte maximum while retaining the board's
        # configured three-byte response; the next reply must be truncated.
        self.read_dcs(0x04)
        self.pop_reply(bytes.fromhex('87 21 5c 00 18'))
        self.max_return(3)
        self.clear(mask=RX_DONE)
        self.read_dcs(0x04)
        self.assertEqual(self.events(), READ_DONE)
        self.assertTrue(q.irq(28))
        self.pop_reply(bytes.fromhex('87 1c 03 00 16 5c 08 0c df 1b'), mask=RX_DONE)


if __name__ == '__main__':
    unittest.main()
