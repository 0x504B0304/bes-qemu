"""Exercise BEST1700 memory, DMA, UART and timer contracts without firmware.

Run in the environment containing the built QEMU executable:
    MIBAND_BEST1700_QEMU=/path/to/qemu-system-arm \
        python3 -m unittest tests.test_best1700_peripherals_qtest -v

Each test owns its QEMU process and keeps generated images and logs in the
system temporary directory. No running emulator or external firmware is used.
"""

import os
from pathlib import Path
import queue
import json
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest


DMA0 = 0x401A0000
DMA_CH0 = DMA0 + 0x100
DMA_TC_IE = 1 << 15
DMA_TC = 1 << 31
DMA_SRC_INC = 1 << 26
DMA_DST_INC = 1 << 27
UARTS = (0x4000B000, 0x4000C000, 0x40011000)
AON_TIMER0 = 0x40083000
SYS_TIMER0 = 0x40002000
SYS_TIMER_CLOCK = 0x4000005C
FAST_COUNTER_LOW = 0x40000288
FAST_COUNTER_HIGH = 0x4000028C
SLOW_HZ = 16384
FAST_HZ = 6_000_000


class QtestMachine:
    def __init__(self, executable, *, image_payload=None, machine_options="",
                 accel="qtest", extra_args=(), serial_backends=None):
        self.temp = tempfile.TemporaryDirectory(prefix="best1700-qtest-")
        self.directory = Path(self.temp.name)
        self.stderr_path = self.directory / "qemu.log"
        self.stderr = self.stderr_path.open("wb")
        image = self.directory / "synthetic-ap.bin"
        if image_payload is None:
            image_payload = (struct.pack("<II", 0x203FFFF0, 0x101C0009) +
                             b"\xfe\xe7" + bytes(0x1000))
        image.write_bytes(image_payload)
        self.serial_paths = [self.directory / f"uart{i}.bin" for i in range(3)]
        self.qmp_path = self.directory / "qmp.sock"
        self.qmp_tcp = os.name == 'nt'
        if self.qmp_tcp:
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                self.qmp_endpoint = reservation.getsockname()
            qmp_address = f'tcp:127.0.0.1:{self.qmp_endpoint[1]},server=on,wait=off'
        else:
            self.qmp_endpoint = str(self.qmp_path)
            qmp_address = f'unix:{self.qmp_path},server=on,wait=off'
        self.qmp_socket = None
        self.qmp_stream = None
        command = [str(executable), "-M", "miband-best1700" + machine_options,
                   "-kernel", str(image), "-display", "none", "-monitor", "none",
                   "-accel", accel, "-qtest", "stdio", "-qmp",
                   qmp_address]
        command += list(extra_args)
        for backend in (serial_backends if serial_backends is not None else
                        [f"file:{path}" for path in self.serial_paths]):
            command += ["-serial", backend]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.stderr,
                                        text=True, bufsize=1)
        self.lines = queue.Queue()
        self.irq_levels = {}
        self.reader = threading.Thread(target=self._read_stdout, daemon=True)
        self.reader.start()

    def _read_stdout(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip())
        self.lines.put(None)

    def command(self, line):
        try:
            self.process.stdin.write(line + "\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise RuntimeError(self.diagnostic(line)) from error
        while True:
            try:
                response = self.lines.get(timeout=5)
            except queue.Empty as error:
                raise TimeoutError(self.diagnostic(line)) from error
            if response is None:
                raise RuntimeError(self.diagnostic(line))
            if response.startswith("IRQ "):
                _, action, number = response.split()
                self.irq_levels[int(number)] = action == "raise"
                continue
            if not response.startswith("OK"):
                raise RuntimeError(f"QTest {line!r}: {response!r}")
            fields = response.split(maxsplit=1)
            return fields[1] if len(fields) == 2 else None

    def diagnostic(self, command):
        return (f"QTest failed or timed out: {command!r}; "
                f"QEMU return code={self.process.poll()}\n" +
                self.stderr_path.read_text(errors="replace")[-8000:])

    def read32(self, address):
        return int(self.command(f"readl {address:#x}"), 0)

    def write32(self, address, value):
        self.command(f"writel {address:#x} {value:#x}")

    def write(self, address, payload):
        self.command(f"write {address:#x} {len(payload)} 0x{payload.hex()}")

    def read(self, address, size):
        return bytes.fromhex(self.command(f"read {address:#x} {size}")[2:])

    def step(self, nanoseconds):
        self.command(f"clock_step {nanoseconds}")

    def irq(self, number):
        # Synchronize qtest's asynchronous notifications. This observes the
        # actual ARMv7M interrupt input, including interrupts while disabled
        # in the NVIC; system-memory reads cannot inspect its private NVIC.
        self.read32(DMA0)
        return self.irq_levels.get(number, False)

    def serial(self, number):
        return self.serial_paths[number].read_bytes()

    def qmp(self, execute, arguments=None):
        if self.qmp_stream is None:
            self.qmp_socket = socket.socket(socket.AF_INET if self.qmp_tcp else socket.AF_UNIX,
                                            socket.SOCK_STREAM)
            self.qmp_socket.settimeout(5)
            deadline = time.monotonic() + 5
            while True:
                try:
                    self.qmp_socket.connect(self.qmp_endpoint)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    if self.process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError(self.diagnostic("QMP connect"))
                    time.sleep(0.01)
            self.qmp_stream = self.qmp_socket.makefile("rwb")
            greeting = json.loads(self.qmp_stream.readline())
            if "QMP" not in greeting:
                raise RuntimeError(f"Unexpected QMP greeting: {greeting}")
            self.qmp("qmp_capabilities")
        request = {"execute": execute}
        if arguments is not None:
            request["arguments"] = arguments
        self.qmp_stream.write(json.dumps(request).encode() + b"\n")
        self.qmp_stream.flush()
        while True:
            reply = json.loads(self.qmp_stream.readline())
            if "error" in reply:
                raise RuntimeError(reply["error"])
            if "return" in reply:
                return reply["return"]

    def close(self):
        self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.reader.join(timeout=1)
        self.process.stdin.close()
        self.process.stdout.close()
        if self.qmp_stream is not None:
            self.qmp_stream.close()
        if self.qmp_socket is not None:
            self.qmp_socket.close()
        self.stderr.close()
        self.temp.cleanup()


@unittest.skipUnless(os.environ.get("MIBAND_BEST1700_QEMU"),
                     "MIBAND_BEST1700_QEMU is not set")
class Best1700PeripheralsQtest(unittest.TestCase):
    def test_aon_semaphores_claim_release_status_and_reset(self):
        base, status = 0x40080100, 0x40080140
        self.assertEqual(self.machine.read32(status), 0xffff)
        for index in range(16):
            address = base + 4 * index
            self.assertEqual(self.machine.read32(address), 1)
            self.assertEqual(self.machine.read32(address), 0)
            self.assertEqual(self.machine.read32(status), 0xffff ^ (1 << index))
            self.machine.write32(address, 0)
            self.assertEqual(self.machine.read32(address), 0)
            self.machine.write32(address, 1)
            self.assertEqual(self.machine.read32(address), 1)
            self.assertEqual(self.machine.read32(status), 0xffff ^ (1 << index))
            self.machine.write32(address, 1)
            self.assertEqual(self.machine.read32(status), 0xffff)
        self.machine.write32(status, 0xffffffff)
        self.assertEqual(self.machine.read32(status), 0xffff)
        self.machine.qmp("system_reset")
        self.machine.qmp("query-status")
        self.assertEqual(self.machine.read32(status), 0xffff)

    def test_bus_low_power_ack_tracks_requests_and_reset(self):
        q = self.machine
        request, acknowledge = 0x400001A0, 0x400001AC
        self.assertEqual(q.read32(acknowledge), 0)
        for bits, expected in ((8, 8), (32, 32), (128, 128), (168, 168),
                               (1 << 11, 1 << 11), (1 << 26, 1 << 27),
                               ((1 << 26) | (1 << 11), (1 << 27) | (1 << 11)),
                               (1 << 25, 1 << 25),
                               (0, 0)):
            q.write32(request, bits)
            self.assertEqual(q.read32(acknowledge), expected)
        q.write32(acknowledge, 0xffffffff)
        self.assertEqual(q.read32(acknowledge), 0)
        q.write32(request, 0xffffffff)
        self.assertEqual(q.read32(acknowledge),
                         168 | (1 << 11) | (1 << 25) | (1 << 27))
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertEqual(q.read32(acknowledge), 0)

    def test_system_semaphores_claim_release_independently_and_reset(self):
        q = self.machine
        status = 0x400000e0
        self.assertEqual(q.read32(status), 0xff)
        for index in range(8):
            address = 0x400000C0 + 4 * index
            self.assertEqual(q.read32(address), 1)
            self.assertEqual(q.read32(address), 0)
            q.write32(address, 0)
            self.assertEqual(q.read32(address), 0)
            self.assertEqual(q.read32(status), 0xff ^ (1 << index))
            q.write32(address, 1)
            self.assertEqual(q.read32(address), 1)
            self.assertEqual(q.read32(status), 0xff ^ (1 << index))
            q.write32(address, 1)
            self.assertEqual(q.read32(status), 0xff)
        q.qmp("system_reset")
        q.qmp("query-status")
        self.assertEqual(q.read32(status), 0xff)
        for index in range(8):
            self.assertEqual(q.read32(0x400000C0 + 4 * index), 1)

    def setUp(self):
        self.machine = QtestMachine(os.environ["MIBAND_BEST1700_QEMU"])
        self.addCleanup(self.machine.close)
        self.machine.command("irq_intercept_in /machine/core[0]")

    def start_dma(self, source, destination, count, control, config):
        q = self.machine
        q.write32(DMA0 + 0x30, 9)
        for offset, value in ((0, source), (4, destination), (8, 0),
                              (12, control | count), (16, config | 1)):
            q.write32(DMA_CH0 + offset, value)

    def assert_dma_idle(self):
        q = self.machine
        self.assertEqual(q.read32(DMA0 + 0x1C) & 1, 0)
        self.assertEqual(q.read32(DMA_CH0 + 0x10) & 1, 0)
        self.assertEqual(q.read32(DMA_CH0 + 0x0C) & 0xFFF, 0)
        self.assertEqual(q.read32(DMA_CH0 + 8), 0)

    def assert_ticks(self, actual, expected):
        # A read at a fractional clock boundary can be rounded to either
        # adjacent tick. This is a timebase check, not a ptimer-policy check.
        self.assertLessEqual(abs(actual - expected), 1,
                             f"expected {expected} ticks within one tick, got {actual}")

    def test_dtcm_core_and_axi_views_share_storage_both_ways(self):
        q = self.machine
        core, axi = 0x20001000, 0x21001000
        payload = bytes(range(32))
        q.write(core, payload)
        self.assertEqual(q.read(axi, len(payload)), payload)
        reverse = bytes(reversed(payload))
        q.write(axi + 7, reverse)
        self.assertEqual(q.read(core + 7, len(reverse)), reverse)
        self.assertEqual(q.read(core, 7), payload[:7])

    def test_dma_copy_residual_count_tc_mask_and_w1c_irq37(self):
        q = self.machine
        payload = bytes(range(32))
        q.write(0x20002000, payload)
        q.write(0x20003000, bytes([0xA5]) * len(payload))
        control = DMA_TC | DMA_SRC_INC | DMA_DST_INC | (2 << 18) | (2 << 21)
        self.start_dma(0x21002000, 0x21003000, len(payload) // 4, control, 0)
        self.assertEqual(q.read32(DMA0 + 0x1C) & 1, 1)
        self.assertEqual(q.read(0x20003000, len(payload)), bytes([0xA5]) * len(payload))
        q.step(1000)
        self.assertEqual(q.read(0x20003000, len(payload)), payload)
        self.assert_dma_idle()
        self.assertEqual(q.read32(DMA0 + 0x14), 1, "TC must latch while masked")
        self.assertEqual(q.read32(DMA0 + 4), 0)
        self.assertEqual(q.read32(DMA0), 0)
        self.assertFalse(q.irq(37))

        q.write32(DMA_CH0 + 0x10, DMA_TC_IE)
        self.assertEqual(q.read32(DMA0 + 4), 1)
        self.assertEqual(q.read32(DMA0), 1)
        self.assertTrue(q.irq(37), "unmask must assert the real DMA0 interrupt")
        q.write32(DMA0 + 8, 2)
        self.assertEqual(q.read32(DMA0 + 0x14), 1, "W1C must be per channel")
        self.assertTrue(q.irq(37))
        q.write32(DMA_CH0 + 0x10, 0)
        self.assertFalse(q.irq(37))
        self.assertEqual(q.read32(DMA0 + 0x14), 1)
        q.write32(DMA_CH0 + 0x10, DMA_TC_IE)
        self.assertTrue(q.irq(37))
        q.write32(DMA0 + 8, 1)
        self.assertEqual(q.read32(DMA0 + 0x14), 0)
        self.assertEqual(q.read32(DMA0 + 4), 0)
        self.assertEqual(q.read32(DMA0), 0)
        self.assertFalse(q.irq(37), "acknowledgement must lower the level")

        # A descriptor without TC requested still transfers and becomes idle.
        q.write(0x20002000, bytes(reversed(payload)))
        self.start_dma(0x21002000, 0x21003000, len(payload) // 4,
                       control & ~DMA_TC, DMA_TC_IE)
        q.step(1000)
        self.assertEqual(q.read(0x20003000, len(payload)), bytes(reversed(payload)))
        self.assert_dma_idle()
        self.assertEqual(q.read32(DMA0 + 0x14), 0)
        self.assertFalse(q.irq(37))

    def test_pl011_uart_tx_enable_low_byte_and_empty_rx(self):
        q = self.machine
        for index, uart in enumerate(UARTS):
            with self.subTest(uart=hex(uart)):
                q.write32(uart, ord("x"))
                q.write32(uart + 0x30, 1)
                q.write32(uart, ord("x"))
                self.assertEqual(q.serial(index), b"")
                q.write32(uart + 0x30, 0x101)
                q.write32(uart, 0xDEADBE50)
                q.command(f"writew {uart:#x} 0xab68")
                q.command(f"writeb {uart:#x} 0x21")
                self.assertEqual(q.serial(index), b"Ph!")
                self.assertEqual(q.read32(uart + 0x18) & 0xB9, 0x91)
                self.assertEqual(q.read32(uart), 0, "empty RX must not echo TX")
                self.assertEqual(q.read32(uart + 0x3C), 1 << 5,
                                 "enabled transmitter has an empty FIFO")
                self.assertEqual(q.read32(uart + 0x40), 0)
                q.write32(uart + 0x30, 0)
                q.write32(uart, ord("x"))
                self.assertEqual(q.serial(index), b"Ph!")

    def test_dma_fixed_uart_destination_preserves_byte_order(self):
        q = self.machine
        payload = b"BEST1700 DMA console\r\n"
        q.write32(UARTS[0] + 0x30, 0x101)
        q.write(0x20004000, payload)
        self.start_dma(0x21004000, UARTS[0], len(payload),
                       DMA_TC | DMA_SRC_INC, DMA_TC_IE | (1 << 11))
        q.step(1000)
        self.assertEqual(q.serial(0), payload)
        self.assert_dma_idle()
        self.assertEqual(q.read32(DMA0 + 0x0C), 0)
        self.assertTrue(q.irq(37))
        q.write32(DMA0 + 8, 1)
        self.assertFalse(q.irq(37))

    def test_dma_two_descriptors_only_terminal_requests_tc(self):
        q = self.machine
        first = b"first DMA segment"
        second = b"second DMA segment"
        q.write(0x20008000, first)
        q.write(0x20008100, second)
        control = DMA_SRC_INC | DMA_DST_INC
        q.write(0x20007000, struct.pack("<IIII", 0x21008100, 0x21009100,
                                      0, control | DMA_TC | len(second)))
        q.write32(DMA0 + 0x30, 9)
        for offset, value in ((0, 0x21008000), (4, 0x21009000), (8, 0x21007001),
                              (12, control | len(first)), (16, DMA_TC_IE | 1)):
            q.write32(DMA_CH0 + offset, value)
        self.assertEqual(q.read32(DMA0 + 0x14), 0)
        q.command("clock_step")
        self.assertEqual(q.read(0x20009000, len(first)), first)
        self.assertEqual(q.read(0x20009100, len(second)), bytes(len(second)))
        self.assertEqual(q.read32(DMA0 + 0x1C) & 1, 1)
        self.assertEqual(q.read32(DMA0 + 0x14), 0)
        self.assertFalse(q.irq(37), "first descriptor did not request TC")
        q.command("clock_step")
        self.assertEqual(q.read(0x20009100, len(second)), second)
        self.assert_dma_idle()
        self.assertEqual(q.read32(DMA0 + 0x14), 1,
                         "TC comes from the final descriptor, not the first control word")
        self.assertEqual(q.read32(DMA0 + 0x18), 0)
        self.assertTrue(q.irq(37))
        q.write32(DMA0 + 8, 1)
        self.assertFalse(q.irq(37))

    def test_dma_unmapped_lli_reports_error_without_terminal_count(self):
        q = self.machine
        q.write(0x20008000, b"before bad LLI")
        q.write32(DMA0 + 0x30, 9)
        for offset, value in ((0, 0x21008000), (4, 0x21009000), (8, 0x70000000),
                              (12, DMA_SRC_INC | DMA_DST_INC | 14),
                              (16, DMA_TC_IE | (1 << 14) | 1)):
            q.write32(DMA_CH0 + offset, value)
        q.step(1_000_000)
        self.assertEqual(q.read32(DMA0 + 0x1C) & 1, 0)
        self.assertEqual(q.read32(DMA_CH0 + 0x10) & 1, 0)
        self.assertEqual(q.read32(DMA0 + 0x14), 0)
        self.assertEqual(q.read32(DMA0 + 0x18), 1)
        self.assertEqual(q.read32(DMA0 + 0x0C), 1)
        self.assertEqual(q.read32(DMA0), 1)
        self.assertTrue(q.irq(37))
        q.write32(DMA0 + 0x10, 2)
        self.assertTrue(q.irq(37), "clearing another channel must retain this error")
        q.write32(DMA0 + 0x10, 1)
        self.assertEqual(q.read32(DMA0 + 0x18), 0)
        self.assertEqual(q.read32(DMA0 + 0x0C), 0)
        self.assertFalse(q.irq(37))

    def test_aon_dual_counter_rate_alarm_and_irq59(self):
        q = self.machine
        free, alarm = AON_TIMER0, AON_TIMER0 + 0x20
        # Two counters in the same aperture must keep independent load,
        # enable, pending, and IRQ state. The first is a 32-bit free runner.
        q.write32(free + 8, 2)
        q.write32(free, 0xFFFFFFFF)
        q.write32(free + 8, 0x82)
        initial = q.read32(free + 4)
        q.write32(alarm + 8, 2)
        q.write32(alarm, SLOW_HZ - 1)
        q.write32(alarm + 8, 0xA3)
        q.step(500_000_000)
        self.assert_ticks((initial - q.read32(free + 4)) & 0xFFFFFFFF, SLOW_HZ // 2)
        self.assert_ticks(q.read32(alarm + 4), SLOW_HZ // 2)
        self.assertEqual(q.read32(alarm + 0x10), 0)
        self.assertFalse(q.irq(59))
        q.step(499_900_000)
        self.assertEqual(q.read32(alarm + 0x10), 0, "alarm fired early")
        q.step(100_000)
        self.assertEqual(q.read32(alarm + 4), 0)
        self.assertEqual(q.read32(alarm + 0x10), 1)
        self.assertEqual(q.read32(alarm + 0x14), 1)
        self.assertTrue(q.irq(59), "second AON counter must drive IRQ59")
        self.assertFalse(q.irq(58), "second counter must not interrupt as counter 0")
        self.assert_ticks((initial - q.read32(free + 4)) & 0xFFFFFFFF, SLOW_HZ)
        q.write32(alarm + 0x0C, 1)
        self.assertEqual(q.read32(alarm + 0x10), 0)
        self.assertEqual(q.read32(alarm + 0x14), 0)
        self.assertFalse(q.irq(59))
        q.step(1_000_000_000)
        self.assertEqual(q.read32(alarm + 4), 0)
        self.assertEqual(q.read32(alarm + 0x10), 0, "one-shot must not rearm itself")
        self.assertFalse(q.irq(59))
        self.assert_ticks((initial - q.read32(free + 4)) & 0xFFFFFFFF, 2 * SLOW_HZ)

    def test_aon_load_plus_one_clocks_before_first_and_periodic_irq(self):
        q = self.machine
        timer = AON_TIMER0 + 0x20
        # Firmware programs ticks - 1: Load=82 must wait 83 source clocks,
        # including the final interval in which Value already reads zero.
        before = 82 * 1_000_000_000 // SLOW_HZ
        deadline = (83 * 1_000_000_000 + SLOW_HZ - 1) // SLOW_HZ
        for control in (0xA3, 0xE2):
            with self.subTest(control=hex(control)):
                q.write32(timer + 8, 2)
                q.write32(timer + 0x0C, 1)
                q.write32(timer, 82)
                self.assertEqual(q.read32(timer + 4), 82)
                q.write32(timer + 8, control)
                q.step(before)
                self.assertEqual(q.read32(timer + 0x10), 0, "Load clocks is one clock early")
                self.assertFalse(q.irq(59))
                q.step(deadline - before)
                self.assertEqual(q.read32(timer + 0x10), 1)
                self.assertTrue(q.irq(59))
                q.write32(timer + 0x0C, 1)
                q.step(before)
                self.assertEqual(q.read32(timer + 0x10), 0)
                q.step(deadline - before)
                self.assertEqual(q.read32(timer + 0x10), int(control == 0xE2))
                self.assertEqual(q.irq(59), control == 0xE2)

    def test_aon_zero_load_expires_after_one_clock(self):
        q = self.machine
        timer = AON_TIMER0 + 0x20
        q.write32(timer + 8, 2)
        q.write32(timer, 0)
        self.assertEqual(q.read32(timer + 4), 0)
        q.write32(timer + 8, 0xA3)
        self.assertEqual(q.read32(timer + 0x10), 0)
        self.assertFalse(q.irq(59), "zero load must not trigger immediately")
        period = (1_000_000_000 + SLOW_HZ - 1) // SLOW_HZ
        q.step(period - 100)
        self.assertEqual(q.read32(timer + 4), 0)
        self.assertEqual(q.read32(timer + 0x10), 0)
        q.step(100)
        self.assertEqual(q.read32(timer + 0x10), 1)
        self.assertTrue(q.irq(59))

    def test_aon_maximum_counts_survive_stop_and_resume(self):
        q = self.machine
        timer = AON_TIMER0 + 0x20
        for width, maximum in ((2, 0xFFFFFFFF), (0, 0xFFFF)):
            with self.subTest(bits=32 if width else 16):
                q.write32(timer + 8, width)
                q.write32(timer + 0x0C, 1)
                q.write32(timer, 0xFFFFFFFF)
                self.assertEqual(q.read32(timer + 4), maximum)
                q.write32(timer + 8, 0xA1 | width)
                self.assertEqual(q.read32(timer + 4), maximum)
                # The internal full-width count is 2**32 (or 2**16),
                # so masking it directly would silently turn it into zero.
                for _ in range(2):
                    q.write32(timer + 8, 0x21 | width)
                    self.assertEqual(q.read32(timer + 4), maximum)
                    q.step(1_000_000_000)
                    self.assertEqual(q.read32(timer + 4), maximum)
                    q.write32(timer + 8, 0xA1 | width)
                    self.assertEqual(q.read32(timer + 4), maximum)
                    self.assertFalse(q.irq(59))
                # One ns beyond 64 clocks avoids a fractional-boundary
                # ptimer read being rounded to the previous tick.
                q.step(64 * 1_000_000_000 // SLOW_HZ + 1)
                remaining = maximum - 64
                self.assertEqual(q.read32(timer + 4), remaining)
                q.write32(timer + 8, 0x21 | width)
                q.step(1_000_000_000)
                self.assertEqual(q.read32(timer + 4), remaining)
                q.write32(timer + 8, 0xA1 | width)
                self.assertEqual(q.read32(timer + 4), remaining)
                q.step(64 * 1_000_000_000 // SLOW_HZ + 1)
                self.assertEqual(q.read32(timer + 4), remaining - 64)
                self.assertEqual(q.read32(timer + 0x10), 0)
                self.assertFalse(q.irq(59))

    def test_fast_counter_tracks_virtual_time_and_carries_into_high_word(self):
        q = self.machine

        def read_counter():
            return q.read32(FAST_COUNTER_LOW) | (q.read32(FAST_COUNTER_HIGH) << 32)

        initial = read_counter()
        for _ in range(4):
            self.assertEqual(read_counter(), initial, "register reads must not advance time")
        q.step(250_000_000)
        self.assertEqual(read_counter() - initial, FAST_HZ // 4)
        q.step(750_000_000)
        self.assertEqual(read_counter() - initial, FAST_HZ)
        q.write32(0x40000290, 0xFFFFFFFE)
        q.write32(0x40000294, 0x11223344)
        self.assertEqual(read_counter(), 0x11223344FFFFFFFE)
        q.step(500)
        self.assertEqual(read_counter(), 0x1122334500000001)

    def test_sys_timer_clock_mux_and_divider_apply_to_both_counters(self):
        q = self.machine
        counters = (SYS_TIMER0, SYS_TIMER0 + 0x20)
        for timer in counters:
            q.write32(timer + 8, 2)
            q.write32(timer, 0xFFFFFFFF)
            q.write32(timer + 8, 0x82)
        for select, rate in ((0, SLOW_HZ), (1 << 17, FAST_HZ),
                             ((1 << 17) | (1 << 21), FAST_HZ // 2),
                             ((1 << 17) | (2 << 21), FAST_HZ // 4),
                             (0, SLOW_HZ)):
            with self.subTest(clock_select=hex(select), rate=rate):
                before_switch = [q.read32(timer + 4) for timer in counters]
                q.write32(SYS_TIMER_CLOCK, select)
                after_switch = [q.read32(timer + 4) for timer in counters]
                for before, after in zip(before_switch, after_switch):
                    self.assert_ticks((before - after) & 0xFFFFFFFF, 0)
                q.step(250_000_000)
                for timer, before in zip(counters, after_switch):
                    self.assert_ticks((before - q.read32(timer + 4)) & 0xFFFFFFFF,
                                      rate // 4)
        self.assertFalse(q.irq(66))
        self.assertFalse(q.irq(67))

    def test_periodic_bgload_changes_next_period_and_mask_retains_pending(self):
        q = self.machine
        timer = AON_TIMER0
        q.write32(timer + 8, 2)
        q.write32(timer, 1023)  # 1024 clocks: 62.5 ms at the slow clock.
        q.write32(timer + 8, 0xE2)
        q.step(31_250_000)
        before_bgload = q.read32(timer + 4)
        q.write32(timer + 0x18, 2047)  # 2048 clocks: next period becomes 125 ms.
        self.assert_ticks(q.read32(timer + 4), before_bgload)
        q.step(30_000_000)
        self.assertEqual(q.read32(timer + 0x10), 0)
        q.step(2_000_000)
        self.assertEqual(q.read32(timer + 0x10), 1)
        self.assertTrue(q.irq(58))
        self.assertGreater(q.read32(timer + 4), 2000, "BGLoad must apply on reload")
        q.write32(timer + 0x0C, 1)
        self.assertFalse(q.irq(58))
        q.step(120_000_000)
        self.assertEqual(q.read32(timer + 0x10), 0, "new period fired early")
        q.step(5_000_000)
        self.assertEqual(q.read32(timer + 0x10), 1)
        self.assertTrue(q.irq(58))
        q.write32(timer + 8, 0xC2)  # Mask without clearing the raw event.
        self.assertEqual(q.read32(timer + 0x10), 1)
        self.assertEqual(q.read32(timer + 0x14), 0)
        self.assertFalse(q.irq(58))
        q.write32(timer + 8, 0xE2)
        self.assertEqual(q.read32(timer + 0x14), 1)
        self.assertTrue(q.irq(58))
        q.write32(timer + 8, 2)
        q.write32(timer + 0x0C, 1)
        stopped = q.read32(timer + 4)
        q.step(1_000_000_000)
        self.assertEqual(q.read32(timer + 4), stopped)
        self.assertEqual(q.read32(timer + 0x10), 0)
        self.assertFalse(q.irq(58))

    def test_reset_clears_pending_irq_and_cancels_scheduled_work(self):
        q = self.machine
        pending, future = AON_TIMER0, AON_TIMER0 + 0x20
        q.write32(pending + 8, 2)
        q.write32(pending, 1024)
        q.write32(pending + 8, 0xA3)
        q.step(63_000_000)
        self.assertTrue(q.irq(58))
        q.write32(future + 8, 2)
        q.write32(future, SLOW_HZ)
        q.write32(future + 8, 0xA3)
        q.write32(SYS_TIMER_CLOCK, (1 << 17) | (1 << 21))
        q.write32(SYS_TIMER0 + 8, 2)
        q.write32(SYS_TIMER0, FAST_HZ)
        q.write32(SYS_TIMER0 + 8, 0xA3)
        q.write(0x20005000, b"reset cancellation")
        q.write(0x20006000, bytes(18))
        self.start_dma(0x21005000, 0x21006000, 18,
                       DMA_TC | DMA_SRC_INC | DMA_DST_INC, DMA_TC_IE)
        self.assertEqual(q.read32(DMA0 + 0x1C), 1)
        q.qmp("system_reset")
        q.qmp("query-status")
        q.step(3_000_000_000)
        self.assertEqual(q.read(0x20006000, 18), bytes(18), "reset must cancel queued DMA")
        self.assertEqual(q.read32(DMA0 + 0x1C), 0)
        self.assertEqual(q.read32(DMA0 + 0x14), 0)
        self.assertEqual(q.read32(SYS_TIMER_CLOCK), 0)
        for timer, irq in ((pending, 58), (future, 59), (SYS_TIMER0, 66)):
            with self.subTest(timer=hex(timer)):
                self.assertEqual(q.read32(timer + 8) & 0x80, 0)
                self.assertEqual(q.read32(timer + 0x10), 0)
                self.assertEqual(q.read32(timer + 0x14), 0)
                self.assertFalse(q.irq(irq))
        self.assertFalse(q.irq(37))


if __name__ == "__main__":
    unittest.main()
