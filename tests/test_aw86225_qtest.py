"""Exercise the haptic slave through BTH I2C1 and virtual-time events."""
import os
import unittest

from tests.test_best1700_i2c_qtest import (
    I2cTestCase, DATA_CMD, CMD_READ, CMD_RESTART, CMD_STOP, ABRT_SOURCE, CLR_INTR,
)
from tests.test_best1700_peripherals_qtest import QtestMachine

BASE = 0x50007000


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Aw86225Qtest(I2cTestCase):
    def setUp(self):
        self.machine = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
            machine_options=',bth-real-boot=on', extra_args=('-device',
                'aw86225,id=test-haptic,bus=/best1700-bth-i2c1/i2c,address=0x58,'
                'reset-target=/machine/gpio,reset-pin=68'))
        self.addCleanup(self.machine.close)
        self.machine.command('irq_intercept_out /machine/peripheral/test-haptic')
        self.configure(base=BASE, target=0x58)

    def write_regs(self, reg, data, accepted=True):
        q = self.machine
        q.read32(BASE + CLR_INTR)
        q.write32(BASE + DATA_CMD, reg)
        for i, byte in enumerate(data):
            q.write32(BASE + DATA_CMD, byte | (CMD_STOP if i == len(data)-1 else 0))
        self.assertEqual(q.read32(BASE + ABRT_SOURCE), 0 if accepted else 8)
        q.read32(BASE + CLR_INTR)

    def read_regs(self, reg, count=1):
        q = self.machine
        q.write32(BASE + DATA_CMD, reg)
        for i in range(count):
            q.write32(BASE + DATA_CMD, CMD_READ | (CMD_RESTART if i == 0 else 0) |
                        (CMD_STOP if i == count-1 else 0))
        self.assertEqual(q.read32(BASE + ABRT_SOURCE), 0)
        return bytes(q.read32(BASE + DATA_CMD) for _ in range(count))

    def ram_address(self, address):
        self.write_regs(0x40, [address >> 8, address & 255])

    def prepare_wave(self, loop=0):
        self.write_regs(0x43, [8])
        self.ram_address(0)
        # One synthetic waveform: inclusive samples 5..244, 10 ms at 24 kHz.
        self.write_regs(0x42, [0x55, 0, 5, 0, 244])
        self.write_regs(0x43, [0])
        self.write_regs(0x44, [2])
        self.write_regs(0x08, [0])
        self.write_regs(0x0a, [1, 0])
        self.write_regs(0x12, [loop << 4])
        self.write_regs(0x03, [0xdf])

    def test_burst_config_ram_fifo_and_bus_isolation(self):
        self.assertEqual(self.read_regs(0), b'\0')
        self.write_regs(0x0a, [1, 2, 3, 4])
        self.assertEqual(self.read_regs(0x0a, 4), bytes([1, 2, 3, 4]))
        self.write_regs(0x43, [8])
        self.ram_address(254)
        payload = bytes([0, 1, 127, 128, 255, 2, 3, 4])
        self.write_regs(0x42, payload)
        self.assertEqual(self.read_regs(0x40, 2), b'\x01\x06')
        self.ram_address(254)
        self.assertEqual(self.read_regs(0x42, 8), payload)
        self.write_regs(0x43, [0])
        self.write_regs(0x42, [42], accepted=False)
        self.assertEqual(self.read_regs(0x42), b'\xff')
        self.configure(base=0x40007000, target=0x58)
        self.machine.write32(0x40007000 + DATA_CMD, CMD_READ | CMD_STOP)
        self.assertEqual(self.machine.read32(0x40007000 + ABRT_SOURCE), 1)

    def test_completion_irq_latch_mask_and_stop_cancels_event(self):
        q = self.machine
        self.prepare_wave()
        self.assertEqual(self.read_regs(2), b'\0')
        self.assertTrue(q.irq(0))  # Active-low output is initially deasserted.
        self.write_regs(9, [1])
        self.assertEqual(self.read_regs(0x3f), b'\x07')
        q.step(9_000_000)
        self.assertEqual(self.read_regs(0x3f), b'\x07')
        q.step(1_000_000)
        self.assertEqual(self.read_regs(0x3f), b'\0')
        self.assertFalse(q.irq(0))
        self.write_regs(3, [255])
        self.assertTrue(q.irq(0))
        self.write_regs(3, [0xdf])
        self.assertFalse(q.irq(0))
        self.assertEqual(self.read_regs(2), b'\x20')
        self.assertTrue(q.irq(0))
        self.assertEqual(self.read_regs(2), b'\0')
        self.write_regs(9, [1])
        q.step(5_000_000)
        self.write_regs(9, [2])
        q.step(20_000_000)
        self.assertEqual(self.read_regs(2), b'\0')
        self.assertEqual(self.read_regs(0x3f), b'\0')

    def test_loop_duration_infinite_loop_and_invalid_wave(self):
        q = self.machine
        self.prepare_wave(loop=2)
        self.write_regs(9, [1])
        q.step(29_000_000)
        self.assertEqual(self.read_regs(0x3f), b'\x07')
        q.step(1_000_000)
        self.assertEqual(self.read_regs(0x3f), b'\0')
        self.read_regs(2)
        self.write_regs(0x12, [0xf0])
        self.write_regs(9, [1])
        q.step(1_000_000_000)
        self.assertEqual(self.read_regs(0x3f), b'\x07')
        self.write_regs(9, [2])
        self.assertEqual(self.read_regs(2), b'\0')
        self.write_regs(0x0a, [127])
        self.write_regs(9, [1], accepted=False)
        self.assertEqual(self.read_regs(0x3f), b'\0')

    def test_calibration_measurements_are_timed_and_reset_clears_them(self):
        q = self.machine
        self.write_regs(8, [2])
        self.write_regs(0x18, [8])
        self.write_regs(9, [1])
        self.assertEqual(self.read_regs(0x25, 4), bytes(4))
        q.step(100_000_000)
        self.assertEqual(self.read_regs(0x25, 4), b'\x06\x40\x06\x40')
        self.write_regs(0x52, [2])
        self.assertEqual(self.read_regs(0x55), b'\0')
        q.step(20_000_000)
        adc = 4000 * 1024 // 6100
        self.assertEqual(self.read_regs(0x55), bytes([adc >> 2]))
        self.assertEqual(self.read_regs(0x57), bytes([(adc & 3) << 4]))
        self.assertEqual(self.read_regs(0x52), b'\0')
        self.write_regs(0, [0xaa])
        self.assertEqual(self.read_regs(0x25, 4), bytes(4))
        self.write_regs(0, [0x32], accepted=False)
        self.write_regs(0x70, [99], accepted=False)
        self.assertEqual(self.read_regs(0), b'\0')

    def test_wait_sequence_is_a_delay_not_an_invalid_wave_id(self):
        q = self.machine
        self.prepare_wave()
        self.write_regs(0x0a, [1, 0x9d, 1, 0])
        self.write_regs(9, [1])
        # Two 240-sample waves plus a 29 * 128-sample wait at 24 kHz.
        duration = (480 + 29 * 128) * 1_000_000_000 // 24000
        q.step(duration - 1)
        self.assertEqual(self.read_regs(0x3f), b'\x07')
        q.step(1)
        self.assertEqual(self.read_regs(0x3f), b'\0')
        self.assertEqual(self.read_regs(2), b'\x20')

    def test_board_reset_gpio_and_machine_reset_clear_ram_and_timers(self):
        q = self.machine
        self.prepare_wave()
        self.write_regs(9, [1])
        # Bank 2 pin 4 = GPIO68. Direction output drives active-low reset.
        q.write32(0x4008a004, 1 << 4)
        q.write32(BASE + DATA_CMD, CMD_READ | CMD_STOP)
        self.assertNotEqual(q.read32(BASE + ABRT_SOURCE), 0)
        q.read32(BASE + CLR_INTR)
        q.write32(0x4008a000, 1 << 4)
        self.assertEqual(self.read_regs(0x3f), b'\0')
        q.step(100_000_000)
        self.assertEqual(self.read_regs(2), b'\0')
        self.write_regs(0x43, [8])
        self.ram_address(0)
        self.assertEqual(self.read_regs(0x42, 5), bytes(5))
        self.prepare_wave()
        self.write_regs(9, [1])
        q.qmp('system_reset')
        q.qmp('query-status')
        self.configure(base=BASE, target=0x58)
        q.step(100_000_000)
        self.assertEqual(self.read_regs(2), b'\0')
        self.assertEqual(self.read_regs(0), b'\0')
        self.write_regs(0x43, [8])
        self.ram_address(0)
        self.assertEqual(self.read_regs(0x42, 5), bytes(5))


if __name__ == '__main__':
    unittest.main()
