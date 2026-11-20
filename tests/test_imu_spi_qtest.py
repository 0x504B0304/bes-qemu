#!/usr/bin/env python3
"""IMU parts on the BEST1503 AP SPI0 bus, driven through real controller
registers: chip-select isolation (CR1[10:7]), LSM6DSO ID/FIFO/INT1 and the
BMI220 config handshake, header FIFO and sensortime frame. Host motion is
injected through the QOM "motion" property on a QMP socket.

Usage: test_imu_spi_qtest.py QEMU   (runs on Windows and POSIX)
"""
import argparse
import json
import socket
import struct
import subprocess
import tempfile
from pathlib import Path

SPI0 = 0x40007000
CR0, CR1, DATA = SPI0, SPI0 + 4, SPI0 + 8
GPIO2 = 0x4008a000                      # pins 64..95
GPIO_COMMON_ENABLE2 = 0x40085080 + 8
G = 9.80665


class Machine:
    def __init__(self, qemu, device):
        self.dir = tempfile.TemporaryDirectory()
        image = Path(self.dir.name) / 'synthetic-ap.bin'
        image.write_bytes(struct.pack('<II', 0x20010000, 0x0c0c0009) + b'\xfe\xe7' + bytes(4096))
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        self.proc = subprocess.Popen(
            [qemu, '-M', 'bes1503', '-kernel', str(image), '-display', 'none',
             '-monitor', 'none', '-accel', 'qtest', '-qtest', 'stdio',
             '-serial', 'none', '-serial', 'none',
             '-qmp', f'tcp:127.0.0.1:{port},server=on,wait=off',
             '-device', device],
            # The qtest protocol log goes to stderr; an unread pipe would block QEMU.
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        self.cmd('clock_step 0')
        self.qmp_sock = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.qmp_file = self.qmp_sock.makefile('rwb')
        json.loads(self.qmp_file.readline())
        self.qmp('qmp_capabilities')

    def cmd(self, line):
        self.proc.stdin.write(line + '\n')
        self.proc.stdin.flush()
        while True:
            reply = self.proc.stdout.readline()
            if not reply:
                raise RuntimeError(f'QEMU exited during {line!r}')
            if not reply.startswith('IRQ'):
                break
        assert reply.startswith('OK'), (line, reply)
        return reply[2:].strip()

    def qmp(self, execute, arguments=None):
        message = {'execute': execute}
        if arguments:
            message['arguments'] = arguments
        self.qmp_file.write(json.dumps(message).encode() + b'\n')
        self.qmp_file.flush()
        while True:
            reply = json.loads(self.qmp_file.readline())
            if 'error' in reply:
                raise RuntimeError(reply['error'])
            if 'return' in reply:
                return reply['return']

    def prop(self, name):
        return self.qmp('qom-get', {'path': '/machine/peripheral/board-imu', 'property': name})

    def motion(self, text):
        self.qmp('qom-set', {'path': '/machine/peripheral/board-imu',
                             'property': 'motion', 'value': text})

    def write32(self, addr, value):
        self.cmd(f'writel {addr:#x} {value:#x}')

    def read32(self, addr):
        return int(self.cmd(f'readl {addr:#x}'), 16)

    def step_ms(self, ms):
        self.cmd(f'clock_step {ms * 1_000_000}')

    def transaction(self, tx, select=0):
        """One CR1-framed transfer on SPI0; returns the received bytes."""
        self.write32(CR0, 0x00070100)                    # 8-bit frames
        self.write32(CR1, 0x02 | (select << 7))
        rx = []
        for byte in tx:
            self.write32(DATA, byte)
            rx.append(self.read32(DATA) & 0xff)
        self.write32(CR1, select << 7)
        return rx

    def close(self):
        self.qmp_file.close()
        self.qmp_sock.close()
        self.proc.kill()
        self.proc.wait()
        self.dir.cleanup()


def enable_gpio_irq(m, pin):
    bit = 1 << (pin - 64)
    m.write32(GPIO_COMMON_ENABLE2, bit)
    m.write32(GPIO2 + 0x1c, bit)


def gpio_pending(m, pin):
    return bool(m.read32(GPIO2 + 0x44) & (1 << (pin - 64)))


def test_lsm6dso(qemu):
    m = Machine(qemu, 'lsm6dso,id=board-imu,bus=miband-ap-spi0,'
                      'irq-target=/machine/gpio,irq-pin=71')
    try:
        reg = lambda r: m.transaction([0x80 | r, 0])[1]
        put = lambda r, v: m.transaction([r, v])
        assert reg(0x0f) == 0x6c, 'WHO_AM_I'
        assert reg(0x12) == 0x04, 'CTRL3_C IF_INC default'
        # Select 1 is the PPG on Band 9: idle-high MISO, IMU untouched.
        assert m.transaction([0x8f, 0], select=1) == [0xff, 0xff]
        m.transaction([0x10, 0x55], select=1)
        assert reg(0x10) == 0x00, 'select-1 write must not reach the IMU'

        enable_gpio_irq(m, 71)
        m.motion(f'{0.5 * G} 0 {G} 0 0 0')
        put(0x07, 4)                     # watermark: 4 words
        put(0x0d, 0x08)                  # INT1_FIFO_TH
        put(0x09, 0x04)                  # BDR_XL 104 Hz
        put(0x0a, 0x06)                  # continuous mode
        put(0x10, 0x40)                  # ODR_XL 104 Hz, +-2 g
        assert m.prop('accel-odr-mhz') == 104000
        assert not gpio_pending(m, 71)
        m.step_ms(30)                    # ~3 samples: below watermark
        assert not gpio_pending(m, 71)
        m.step_ms(30)
        status1, status2 = m.transaction([0xba, 0, 0])[1:]
        level = status1 | (status2 & 3) << 8
        assert level >= 4 and status2 & 0x80, (level, hex(status2))
        assert gpio_pending(m, 71), 'FIFO watermark must reach GPIO71'

        # Two words in one burst: Z_H rolls back to the tag register.
        burst = m.transaction([0xf8] + [0] * 14)[1:]
        for word in (burst[:7], burst[7:]):
            tag = word[0]
            assert tag >> 3 == 0x02, hex(tag)            # accelerometer
            assert bin(tag).count('1') % 2 == 1, 'odd tag parity'
            x, y, z = struct.unpack('<hhh', bytes(word[1:]))
            assert (x, y) == (round(500 / 0.061), 0) and z == round(1000 / 0.061), (x, y, z)
        assert (burst[0] >> 1) & 3 != (burst[7] >> 1) & 3, 'TAG_CNT advances'
        out = struct.unpack('<hhh', bytes(m.transaction([0xa8] + [0] * 6)[1:]))
        assert out == (round(500 / 0.061), 0, round(1000 / 0.061)), out

        put(0x10, 0x00)
        assert m.prop('accel-odr-mhz') == 0
        before = m.prop('samples')
        m.step_ms(50)
        assert m.prop('samples') == before, 'sampling stops with ODR 0'
        put(0x12, 0x05)                  # SW_RESET restores defaults
        assert reg(0x07) == 0 and reg(0x12) == 0x04
    finally:
        m.close()


def test_bmi220(qemu):
    m = Machine(qemu, 'bmi220,id=board-imu,bus=miband-ap-spi0,'
                      'irq-target=/machine/gpio,irq-pin=72')
    try:
        reg = lambda r: m.transaction([0x80 | r, 0, 0])[2]  # dummy byte
        put = lambda r, *v: m.transaction([r, *v])
        assert reg(0x00) == 0x26, 'BMI220 CHIP_ID'
        put(0x7e, 0xb6)                  # soft reset
        assert reg(0x21) & 0x0f == 0, 'not initialised before config load'
        put(0x7c, 0x00)
        put(0x59, 0x00)
        put(0x5b, 0x00, 0x00)
        put(0x5e, *range(64))            # INIT_DATA does not auto-increment
        put(0x59, 0x01)
        assert reg(0x21) & 0x0f == 0x01, 'INTERNAL_STATUS init_ok'
        assert m.prop('init-bytes') == 64
        assert reg(0x59) == 0x01, 'registers after INIT_DATA untouched'

        enable_gpio_irq(m, 72)
        m.motion(f'{0.5 * G} 0 {G} 0 0 0')
        put(0x41, 0x00)                  # +-2 g
        put(0x40, 0xa8)                  # 100 Hz
        put(0x49, 0x50)                  # header mode, accelerometer
        put(0x46, 28, 0)                 # watermark 28 bytes = 4 frames
        put(0x58, 0x02)                  # fwm -> INT1
        put(0x53, 0x0a)                  # INT1 output, active high
        put(0x7d, 0x04)                  # ACC on
        assert m.prop('accel-odr-mhz') == 100000
        m.step_ms(25)
        assert not gpio_pending(m, 72)
        m.step_ms(30)
        length = m.transaction([0xa4, 0, 0, 0])[2:]
        length = length[0] | length[1] << 8
        assert length >= 28 and length % 7 == 0, length
        assert gpio_pending(m, 72), 'FIFO watermark must reach GPIO72'
        data = m.transaction([0xa6, 0] + [0] * (length + 4))[2:]
        frames = [data[i:i + 7] for i in range(0, length, 7)]
        for frame in frames:
            assert frame[0] == 0x84, hex(frame[0])       # accelerometer frame
            x, y, z = struct.unpack('<hhh', bytes(frame[1:]))
            assert (x, y, z) == (8192, 0, 16384), (x, y, z)
        assert data[length] == 0x44, 'sensortime frame after reading empty'
        assert m.transaction([0xa4, 0, 0, 0])[2:] == [0, 0]
        assert m.transaction([0xa6, 0, 0])[2] == 0x80, 'empty FIFO reads 0x80'
    finally:
        m.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('qemu')
    a = p.parse_args()
    for test in (test_lsm6dso, test_bmi220):
        test(a.qemu)
        print(f'{test.__name__}: ok')


if __name__ == '__main__':
    main()
