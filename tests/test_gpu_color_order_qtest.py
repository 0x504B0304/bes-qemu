"""Independent pen-register vs texture/target channel-order regression.

GC NanoUltraV A02 is ABGR; target/CLUT canonical pixels are ARGB. These
asymmetric color oracles must not be derived from the renderer's conversion.
"""
import os
import struct
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine
from tests.test_gpu_bgr24_texture_qtest import pattern, state
from tests.test_best1700_gpu_qtest import GPU, COMMAND, SOURCE, DESTINATION


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'), 'native QEMU is not configured')
class GpuColorOrderQtest(unittest.TestCase):
    def setUp(self):
        self.machine = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'])
        self.addCleanup(self.machine.close)

    def draw(self, color, *, target_format=0x109, mode=0x10000102, rectangle=False,
             source_format=0x01000107):
        q = self.machine
        commands = pattern(1, 1, SOURCE, source_format=source_format)[:-64]
        commands += b''.join(state(r, v) for r, v in (
            (0xA10, target_format), (0xA11, DESTINATION), (0xA00, mode), (0xA02, color)))
        if rectangle:
            commands += struct.pack('<II4H', 0x40000001, 0, 0, 0, 1, 1)
        else:
            from tests.test_gpu_bgr24_texture_qtest import path_rectangle
            commands += path_rectangle(0, 0, 1, 1)
        commands += bytes(8)
        q.write(COMMAND, commands)
        q.write32(GPU + 0x14, 1)
        q.write32(GPU + 0x500, COMMAND)
        q.write32(GPU + 0x504, len(commands) // 8)
        self.assertEqual(q.read32(GPU + 0x10), 1)

    def test_solid_primary_colors_across_packed_targets_and_paths(self):
        cases = ((0x101, b'\x00\xf8', b'\x1f\x00'),
                 (0x105, b'\x00\xfc', b'\x1f\x80'),
                 (0x109, b'\x00\x00\xff', b'\xff\x00\x00'),
                 (0x10a, b'\x00\xf8\xff', b'\x1f\x00\xff'),
                 (0x103, b'\x00\x00\xff\xff', b'\xff\x00\x00\xff'))
        for fmt, red, blue in cases:
            for rectangle in (False, True):
                for color, expected in ((0xff0000ff, red), (0xffff0000, blue)):
                    with self.subTest(format=hex(fmt), rectangle=rectangle, pen=hex(color)):
                        self.machine.write(DESTINATION, bytes(8))
                        self.draw(color, target_format=fmt, rectangle=rectangle)
                        self.assertEqual(self.machine.read(DESTINATION, len(expected)), expected)

    def test_asymmetric_pen_and_premultiplied_alpha(self):
        for color, destination, expected in (
                (0xff3939e5, bytes(3), bytes((57, 57, 229))),
                (0x8014283c, bytes((100, 80, 60)), bytes((70, 80, 90)))):
            self.machine.write(DESTINATION, destination)
            self.draw(color)
            self.assertEqual(self.machine.read(DESTINATION, 3), expected)

    def test_texture_is_not_swapped_and_multiply_uses_abgr_tint(self):
        self.machine.write(SOURCE, bytes((40, 80, 120, 255)))
        self.machine.write(DESTINATION, bytes(4))
        self.draw(0, mode=0x10001102)
        self.assertEqual(self.machine.read(DESTINATION, 3), bytes((40, 80, 120)))
        for rectangle in (False, True):
            self.machine.write(DESTINATION, bytes((100, 80, 60)))
            self.draw(0x808040ff, mode=0x10002101 if rectangle else 0x10002102,
                      rectangle=rectangle)
            self.assertEqual(self.machine.read(DESTINATION, 3), bytes((70, 60, 150)))

    def test_indexed_palette_remains_argb(self):
        self.machine.write(SOURCE, b'\0')
        self.machine.write(COMMAND, state(0xB00, 0xffe53939) + bytes(8))
        self.machine.write32(GPU + 0x500, COMMAND)
        self.machine.write32(GPU + 0x504, 2)
        self.machine.read32(GPU + 0x10)
        self.machine.write(DESTINATION, bytes(4))
        self.draw(0, mode=0x10001102, source_format=0x01000900)
        self.assertEqual(self.machine.read(DESTINATION, 3), bytes((57, 57, 229)))
