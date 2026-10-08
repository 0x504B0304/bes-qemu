"""Physical I2C report/IRQ path driven by QMP absolute pointer events."""
import os
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine
from tests.test_best1700_i2c_qtest import DATA_CMD, CMD_READ, CMD_STOP

I2C = 0x40007000
GPIO = 0x4008a000
PIN = 1 << 22  # GPIO86


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'), 'QEMU required')
class Cst9217Qtest(unittest.TestCase):
    def setUp(self):
        self.q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'], extra_args=(
            '-device', 'cst9217,id=touch,bus=/best1700-i2c1/i2c,address=90,'
            'irq-target=/machine/gpio,irq-pin=86,host-input=on,'
            'reset-target=/machine/gpio,reset-pin=47'))
        self.addCleanup(self.q.close)
        self.q.command('irq_intercept_in /machine/core[0]')
        self.configure()

    def configure(self):
        q = self.q
        q.write32(I2C + 0x6c, 0)
        q.write32(I2C, 0x63)
        q.write32(I2C + 4, 90)
        q.write32(I2C + 0x6c, 1)
        q.write32(GPIO + 0x30, PIN)
        q.write32(GPIO + 0x1c, PIN)
        q.write32(GPIO + 0x20, PIN)  # Low level, as the guest worker masks IRQ.
        q.write32(GPIO + 0x24, PIN)
        q.write32(0x400851c8, PIN)

    def write(self, data):
        for i, byte in enumerate(data):
            self.q.write32(I2C + DATA_CMD, byte | (CMD_STOP if i + 1 == len(data) else 0))

    def read(self, selector, size):
        self.write(bytes.fromhex(selector))
        for i in range(size):
            self.q.write32(I2C + DATA_CMD, CMD_READ | (CMD_STOP if i + 1 == size else 0))
        return bytes(self.q.read32(I2C + DATA_CMD) & 255 for _ in range(size))

    def pointer(self, x, y, down):
        # QMP abs range matches the VNC backend. Machine paused is accepted
        # under qtest; input is explicitly synchronized by the QMP command.
        self.q.qmp('input-send-event', {'events': [
            {'type': 'abs', 'data': {'axis': 'x', 'value': (x * 32767 + 478) // 479}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': (y * 32767 + 478) // 479}},
            {'type': 'btn', 'data': {'button': 'left', 'down': down}},
        ]})

    def test_probe_modes_boot_identity_and_readonly_identity(self):
        self.assertEqual(self.read('d224', 4), bytes.fromhex('1792caca'))
        self.write(bytes.fromhex('d11e'))
        self.assertEqual(self.read('0002', 2), b'\0\x1e')
        self.write(bytes.fromhex('d101'))
        self.assertEqual(self.read('0002', 2), b'\0\1')
        self.write(bytes.fromhex('a001aa'))
        self.assertEqual(self.read('a002', 2), bytes.fromhex('55b0'))
        self.write(bytes.fromhex('a01001'))
        self.write(bytes.fromhex('a00c7c07'))
        self.assertEqual(self.read('a018', 4), bytes.fromhex('1792caca'))
        self.write(bytes.fromhex('d224ffffffff'))
        self.assertEqual(self.read('d224', 4), bytes.fromhex('1792caca'))

    def test_press_move_release_queue_ack_and_reset(self):
        self.assertFalse(self.q.irq(2))
        self.pointer(479, 0, True)
        self.pointer(0, 479, True)
        self.pointer(0, 479, False)
        expected = [(6, 479, 0), (7, 0, 479), (0, 0, 479)]
        for event, x, y in expected:
            self.assertTrue(self.q.irq(2))
            r = self.read('d000', 10)
            self.assertEqual((r[0], r[1] * 16 + (r[3] >> 4),
                              r[2] * 16 + (r[3] & 15), r[5], r[6]),
                             (event, x, y, 1, 0xab))
            self.assertEqual(self.read('d000', 10), r, 'read is not acknowledgment')
            self.write(bytes.fromhex('000500'))
            self.assertFalse(self.q.irq(2))
            self.q.step(1000000)
        self.assertFalse(self.q.irq(2))
        self.pointer(479, 479, True)
        self.assertTrue(self.q.irq(2))
        self.q.qmp('system_reset')
        self.configure()
        self.assertFalse(self.q.irq(2))
        self.pointer(479, 479, True)
        self.assertEqual(self.read('d000', 10)[0], 6)

    def test_deep_sleep_ignores_contacts_until_gpio_reset(self):
        q = self.q
        q.write32(GPIO + 0x38, PIN)  # Firmware uses falling edges.
        self.pointer(100, 100, True)
        self.pointer(100, 100, False)
        self.read('d000', 10)
        self.write(bytes.fromhex('000500'))  # A queued release has a timer.
        q.write32(GPIO + 0x4c, PIN)
        self.write(bytes.fromhex('d105'))
        self.assertEqual(self.read('0002', 2), b'\0\5')
        q.step(2000000)
        self.assertFalse(q.irq(2), 'sleep cancels pending report notification')
        # D105 is deep sleep, not the D104 gesture-recognition mode.
        self.pointer(100, 100, True)
        self.assertFalse(q.irq(2), 'deep sleep must not synthesize a wake press')
        self.pointer(100, 100, False)
        self.assertFalse(q.irq(2))
        self.assertEqual(self.read('d000', 10)[5], 0, 'no stale contact')
        # Original driver uses bank1 GPIO47: low for 5 ms, then high.
        reset_bank, reset_pin = 0x40089000, 1 << 15
        q.write32(reset_bank + 0x00, reset_pin)
        q.write32(reset_bank + 0x04, reset_pin)
        q.write32(reset_bank + 0x0c, reset_pin)
        self.pointer(200, 200, True)
        self.pointer(200, 200, False)  # Contact ended while held in reset.
        q.step(5000000)
        q.write32(reset_bank + 0x00, reset_pin)
        self.pointer(200, 200, False)
        self.assertEqual(self.read('0002', 2), b'\0\0')
        self.assertFalse(q.irq(2))
        self.pointer(150, 160, True)
        self.assertTrue(q.irq(2))
        r = self.read('d000', 10)
        self.assertEqual((r[0], r[1] * 16 + (r[3] >> 4),
                          r[2] * 16 + (r[3] & 15)), (6, 150, 160))
        self.write(bytes.fromhex('000500'))
        q.write32(GPIO + 0x4c, PIN)
        self.assertFalse(q.irq(2))

    def gpio_reset(self, asserted):
        bank, pin = 0x40089000, 1 << 15
        self.q.write32(bank + 0x00, pin)
        self.q.write32(bank + 0x04, pin)
        if asserted:
            self.q.write32(bank + 0x0c, pin)

    def ack_report(self, event, x, y):
        report = self.read('d000', 10)
        self.assertEqual((report[0], report[1] * 16 + (report[3] >> 4),
                          report[2] * 16 + (report[3] & 15), report[5]),
                         (event, x, y, 1))
        self.write(bytes.fromhex('000500'))
        self.q.step(1000000)

    def test_active_contact_survives_gpio_reset_until_actual_release(self):
        self.pointer(120, 130, True)
        self.ack_report(6, 120, 130)
        self.gpio_reset(True)
        self.q.step(5000000)
        self.gpio_reset(False)
        self.ack_report(6, 120, 130)
        self.pointer(120, 130, False)
        self.ack_report(0, 120, 130)
        self.assertFalse(self.q.irq(2))
        self.pointer(210, 220, True)
        self.pointer(210, 220, False)
        self.ack_report(6, 210, 220)
        self.ack_report(0, 210, 220)

    def test_release_while_reset_is_observed_on_rescan(self):
        self.pointer(120, 130, True)
        self.ack_report(6, 120, 130)
        self.gpio_reset(True)
        self.pointer(120, 130, False)
        self.assertFalse(self.q.irq(2))
        self.gpio_reset(False)
        self.ack_report(0, 120, 130)
        self.assertFalse(self.q.irq(2))

    def test_contact_starting_while_reset_is_resampled(self):
        self.gpio_reset(True)
        self.pointer(220, 230, True)
        self.assertFalse(self.q.irq(2))
        self.gpio_reset(False)
        self.ack_report(6, 220, 230)
        self.pointer(220, 230, False)
        self.ack_report(0, 220, 230)

    def tap(self, x=120, y=130, duration_ns=80000000):
        self.pointer(x, y, True)
        self.q.step(duration_ns)
        self.pointer(x, y, False)

    def double_tap(self):
        self.tap()
        self.q.step(120000000)
        self.tap()

    def test_gesture_sleep_double_tap_report_ack_and_resume(self):
        self.write(bytes.fromhex('d104'))
        self.assertEqual(self.read('0002', 2), b'\0\4')
        self.tap()
        self.assertFalse(self.q.irq(2), 'one completed tap is not a double tap')
        self.q.step(120000000)
        self.tap()
        self.assertTrue(self.q.irq(2))
        report = self.read('d000', 10)
        self.assertEqual((report[0], report[4], report[5], report[6]),
                         (0, 0x10, 0, 0xab))
        self.assertEqual(self.read('d000', 10), report)
        self.double_tap()
        self.assertEqual(self.read('d000', 10), report, 'pending gesture retained')
        self.write(bytes.fromhex('000500'))
        self.assertFalse(self.q.irq(2))
        self.q.step(1000000)
        self.assertFalse(self.q.irq(2), 'no extra report queued while pending')
        self.assertEqual(self.read('d000', 10)[4:7], b'\0\0\xab')
        self.double_tap()
        self.assertTrue(self.q.irq(2), 'acknowledgment allows another gesture')
        self.gpio_reset(True)
        self.gpio_reset(False)
        self.assertFalse(self.q.irq(2))
        self.assertEqual(self.read('0002', 2), b'\0\0')
        self.pointer(210, 220, True)
        self.ack_report(6, 210, 220)
        self.pointer(210, 220, False)
        self.ack_report(0, 210, 220)

    def test_gesture_rejects_slow_distant_held_and_dragged_contacts(self):
        # Recognition bounds are configurable host input policy, not measured
        # touch-chip scan timing. Exercise each rejection independently.
        for case in ('gap', 'distance', 'hold', 'drag'):
            with self.subTest(case=case):
                self.write(bytes.fromhex('d104'))
                self.tap()
                self.q.step(501000000 if case == 'gap' else 120000000)
                if case == 'distance':
                    self.tap(200, 200)
                elif case == 'hold':
                    self.tap(duration_ns=251000000)
                elif case == 'drag':
                    self.pointer(120, 130, True)
                    self.pointer(180, 190, True)
                    self.pointer(120, 130, True)
                    self.pointer(120, 130, False)
                else:
                    self.tap()
                self.assertFalse(self.q.irq(2))
                self.assertEqual(self.read('d000', 10)[4:7], b'\0\0\xab')

    def test_gesture_boundaries_and_reset_cancel_partial_recognition(self):
        self.write(bytes.fromhex('d104'))
        self.tap(duration_ns=250000000)
        self.q.step(500000000)
        self.tap(144, 154, duration_ns=250000000)
        self.assertTrue(self.q.irq(2))
        self.read('d000', 10)
        self.write(bytes.fromhex('000500'))
        for reset in ('mode', 'gpio', 'machine'):
            with self.subTest(reset=reset):
                self.write(bytes.fromhex('d104'))
                self.tap()
                if reset == 'mode':
                    self.write(bytes.fromhex('d105'))
                elif reset == 'gpio':
                    self.gpio_reset(True)
                    self.gpio_reset(False)
                else:
                    self.q.qmp('system_reset')
                    self.configure()
                self.write(bytes.fromhex('d104'))
                self.tap()
                self.assertFalse(self.q.irq(2), 'reset discards first tap')
                self.tap()
                self.assertTrue(self.q.irq(2))
                self.read('d000', 10)
                self.write(bytes.fromhex('000500'))


if __name__ == '__main__':
    unittest.main()
