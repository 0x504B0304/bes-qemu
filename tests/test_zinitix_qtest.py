"""Synthetic I2C contracts for double-tap and legacy AF wake protocols."""
import json
import os
from pathlib import Path
import tempfile
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine
from tools.synthetic_config import configuration


@unittest.skipUnless(os.environ.get('MIBAND_TEST_QEMU'), 'native QEMU required')
class ZinitixBus(unittest.TestCase):
    sleep_gesture = 1

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='zinitix-qtest-')
        self.addCleanup(temp.cleanup)
        cfg = configuration()
        cfg['board'].update(touch_model='zinitix', touch_sleep_gesture=self.sleep_gesture,
                            width=192, height=490, touch_irq_gpio=12)
        path = Path(temp.name)/'hardware.json'
        path.write_text(json.dumps(cfg), encoding='utf-8')
        self.q = QtestMachine(os.environ['MIBAND_TEST_QEMU'],
            machine_name='bes1503', machine_options=',hardware-config='+str(path),
            image_payload=bytes.fromhex('fee7'), serial_backends=('none', 'none'))
        self.addCleanup(self.q.close)
        self.bus = 0x40063000
        self.q.write32(self.bus + 4, 32)
        self.q.write32(self.bus + 0x6c, 1)

    def write(self, data):
        for index, byte in enumerate(data):
            self.q.write32(self.bus + 0x10,
                           byte | (0x200 if index == len(data)-1 else 0))
            self.q.step(22500)

    def command(self, register):
        self.write(register.to_bytes(2, 'little'))

    def read(self, register, length=2):
        self.command(register)
        for index in range(length):
            self.q.write32(self.bus + 0x10,
                           0x100 | (0x200 if index == length-1 else 0))
            self.q.step(22500)
        return bytes(self.q.read32(self.bus + 0x10) & 255 for _ in range(length))

    def gesture(self):
        return int.from_bytes(self.read(0x126), 'little')

    def pointer(self, down, x=5, y=5):
        self.q.qmp('input-send-event', {'events': [
            {'type': 'abs', 'data': {'axis': 'x', 'value': x * 32767 // 191}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': y * 32767 // 489}},
            {'type': 'btn', 'data': {'button': 'left', 'down': down}}]})

    def tap(self, *, ack=False, x=5, duration=80000000):
        self.pointer(True, x)
        if ack:
            self.assertEqual(self.read(0x80, 10)[9], 3)
            self.command(3)
        self.q.step(duration)
        self.pointer(False, x)
        if ack:
            self.assertEqual(self.read(0x80, 10)[9], 8)
            self.command(3)


class ZinitixSleepBoundary(ZinitixBus):
    def test_single_then_double_tap_retains_until_ack_and_resumes_contacts(self):
        self.command(5)
        self.tap()
        self.assertEqual(self.gesture(), 0, 'single tap must not wake')
        self.q.step(100000000)
        self.tap()
        self.assertEqual(self.gesture(), 1)
        self.assertEqual(self.gesture(), 1, 'read is not acknowledgement')
        self.assertEqual(self.read(0x80, 10), bytes(10), 'wake taps are not UI contacts')
        self.command(3)
        self.assertEqual(self.gesture(), 0)
        self.command(0xff8)
        self.tap(ack=True)

    def test_completed_awake_tap_and_report_ack_survive_sleep_transition(self):
        self.tap(ack=True)
        self.assertEqual(self.gesture(), 0, 'awake tap cannot emit a wake gesture')
        self.command(5)
        self.q.step(100000000)
        self.tap()
        self.assertEqual(self.gesture(), 1)

    def test_press_straddling_sleep_keeps_one_physical_contact(self):
        self.pointer(True)
        self.assertEqual(self.read(0x80, 10)[9], 3)
        self.command(3)
        self.q.step(40000000)
        self.command(5)
        self.q.step(40000000)
        self.pointer(False)
        self.assertEqual(self.gesture(), 0)
        self.q.step(100000000)
        self.tap()
        self.assertEqual(self.gesture(), 1)

    def test_expired_awake_tap_cannot_pair_after_sleep(self):
        self.tap(ack=True)
        self.command(5)
        self.q.step(501000000)
        self.tap()
        self.assertEqual(self.gesture(), 0)
        self.q.step(100000000)
        self.tap()
        self.assertEqual(self.gesture(), 1)

    def test_long_press_does_not_seed_sleep_double_tap(self):
        self.tap(ack=True, duration=301000000)
        self.command(5)
        self.tap()
        self.assertEqual(self.gesture(), 0)

    def test_reset_clears_awake_tap_history(self):
        self.tap(ack=True)
        self.command(0)
        self.command(5)
        self.tap()
        self.assertEqual(self.gesture(), 0)

    def test_distant_awake_tap_cannot_pair_after_sleep(self):
        self.tap(ack=True, x=5)
        self.command(5)
        self.tap(x=100)
        self.assertEqual(self.gesture(), 0)


class ZinitixAfGestures(ZinitixBus):
    sleep_gesture = 0

    def arm(self, value):
        self.write(bytes.fromhex('af00') + value.to_bytes(2, 'little'))
        self.command(5)

    def test_enabled_single_tap_is_latched_without_ui_contacts(self):
        self.arm(2)
        self.pointer(True)
        self.assertEqual(self.gesture(), 1)
        self.pointer(False)
        self.assertEqual(self.gesture(), 1, 'release cannot clear a pending wake')
        self.assertEqual(self.read(0x80, 10), bytes(10))
        self.assertEqual(self.read(0x1fb), b'\x01\x00')
        self.command(3)
        self.assertEqual(self.gesture(), 0)
        self.command(4)
        self.tap(ack=True)

    def test_slide_up_only_emits_on_valid_completed_stroke(self):
        self.arm(0x10)
        self.pointer(True, 80, 240)
        self.assertEqual(self.gesture(), 0, 'press alone is not an upward stroke')
        self.pointer(False, 80, 160)
        self.assertEqual(self.gesture(), 4)
        self.assertEqual(self.read(0x80, 10), bytes(10))
        self.command(3)
        self.assertEqual(self.gesture(), 0)
        self.pointer(False, 80, 80)
        self.assertEqual(self.gesture(), 0, 'orphan release is not a new stroke')

    def test_slide_mode_rejects_tap_downward_and_horizontal_motion(self):
        self.arm(0x10)
        for start, end in (((80, 240), (80, 240)),
                           ((80, 160), (80, 240)),
                           ((20, 240), (120, 160))):
            with self.subTest(start=start, end=end):
                self.pointer(True, *start)
                self.pointer(False, *end)
                self.assertEqual(self.gesture(), 0)
                self.assertEqual(self.read(0x80, 10), bytes(10))

    def test_unarmed_legacy_board_retains_contact_wake(self):
        self.arm(0)
        self.pointer(True)
        self.assertEqual(self.gesture(), 0)
        self.assertEqual(self.read(0x1fb), bytes(2))
        self.assertEqual(self.read(0x80, 10)[9], 3)
        self.command(3)
        self.pointer(False)
        self.assertEqual(self.read(0x80, 10)[9], 8)


if __name__ == '__main__':
    unittest.main()
