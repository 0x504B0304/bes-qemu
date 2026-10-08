"""Guest LCDC scanout through the BEST1700 address map and DSI panel state.

The color pattern is test input in guest RAM. QMP screenshots must come from
the LCDC transfer and panel GRAM; no firmware or host-drawn watch UI is used.
"""

import os
from pathlib import Path
import struct
import tempfile
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine


LCDC = 0x401C0000
DSI = 0x401D0000
CPU_FB = 0x20160000
AXI_FB = 0x21160000
WIDTH = HEIGHT = 480
STRIDE = WIDTH * 3
FRAME_BYTES = STRIDE * HEIGHT
FRAME_DONE = (1 << 21) | (1 << 27)
BLACK = bytes(FRAME_BYTES)


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700LcdcQtest(unittest.TestCase):
    def machine_options(self):
        return ""

    def setUp(self):
        self.machine = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                                    machine_options=self.machine_options())
        self.addCleanup(self.machine.close)
        self.machine.command('irq_intercept_in /machine/core[0]')
        self.panel_command(0x01)
        self.panel_command(0x11)
        self.panel_command(0x29)
        self.screenshot_index = 0

    def panel_command(self, command):
        self.machine.write32(DSI + 0x18, 0x05 | (command << 8))
        self.machine.write32(DSI + 0x14, 0x5E104)

    def configure(self, address=AXI_FB, *, mask=FRAME_DONE):
        q = self.machine
        for offset, value in ((0xF4, address), (0xFC, STRIDE),
                              (0x104, (HEIGHT << 16) | WIDTH),
                              (0x264, 0x200), (0x1C0, mask)):
            q.write32(LCDC + offset, value)
            self.assertEqual(q.read32(LCDC + offset), value,
                             f'LCDC MMIO wrapper lost register {offset:#x}')
        q.write32(LCDC + 0x1C4, 0)

    def request_frame(self):
        self.machine.write32(LCDC + 0x1C4, 0)
        self.machine.write32(LCDC + 0x224, 1)

    def complete_frame(self):
        q = self.machine
        q.step(20_000_000)
        self.assertEqual(q.read32(LCDC + 0x1C4) & FRAME_DONE, FRAME_DONE)
        self.assertEqual(q.read32(LCDC + 0x224) & 1, 0)

    def assert_screenshot(self, expected):
        q = self.machine
        path = q.directory / f'lcdc-{self.screenshot_index}.ppm'
        self.screenshot_index += 1
        q.qmp('screendump', {'filename': str(path)})
        with path.open('rb') as image:
            self.assertEqual(image.readline(), b'P6\n')
            self.assertEqual(image.readline(), b'480 480\n')
            self.assertEqual(image.readline(), b'255\n')
            pixels = image.read()
        self.assertEqual(len(pixels), FRAME_BYTES)
        if pixels != expected:
            index = next(i for i, pair in enumerate(zip(pixels, expected)) if pair[0] != pair[1])
            self.fail(f'scanout differs at pixel {index // 3}, component {index % 3}: '
                      f'got {pixels[index:index + 12].hex()}, '
                      f'expected {expected[index:index + 12].hex()}')

    def write_color_pattern(self):
        # Format 2 stores 0xRRGGBB pixels as B,G,R bytes on this little-endian
        # bus. Four non-gray quadrants detect offsets, stride and channel order.
        top_source = b'\x00\x00\xff' * 240 + b'\xff\x00\x00' * 240
        bottom_source = b'\x00\xff\x00' * 240 + b'\x56\x34\x12' * 240
        top_rgb = b'\xff\x00\x00' * 240 + b'\x00\x00\xff' * 240
        bottom_rgb = b'\x00\xff\x00' * 240 + b'\x12\x34\x56' * 240
        source = top_source * 240 + bottom_source * 240
        self.machine.write(CPU_FB, source)
        self.assertEqual(self.machine.read(AXI_FB, 12), source[:12])
        self.assertEqual(self.machine.read(AXI_FB + FRAME_BYTES - 12, 12), source[-12:])
        return top_rgb * 240 + bottom_rgb * 240

    def test_axi_alias_rgb888_scanout_and_irq27(self):
        q = self.machine
        expected = self.write_color_pattern()
        self.configure(mask=0)
        self.request_frame()
        self.assertEqual(q.read32(LCDC + 0x224) & 1, 1)
        q.step(10_000_000)
        self.assertEqual(q.read32(LCDC + 0x1C4) & FRAME_DONE, 0)
        self.assert_screenshot(BLACK)
        q.step(10_000_000)
        self.assertEqual(q.read32(LCDC + 0x1C4) & FRAME_DONE, FRAME_DONE)
        self.assertEqual(q.read32(LCDC + 0x224) & 1, 0)
        self.assertFalse(q.irq(27), 'raw completion must respect interrupt masking')
        self.assert_screenshot(expected)
        q.write32(LCDC + 0x1C0, FRAME_DONE)
        self.assertTrue(q.irq(27))
        q.write32(LCDC + 0x1C4, 0)
        self.assertEqual(q.read32(LCDC + 0x1C4) & FRAME_DONE, 0)
        self.assertFalse(q.irq(27), 'firmware acknowledgement must lower IRQ27')

    def test_dsi_sleep_and_display_off_control_same_panel_gram(self):
        q = self.machine
        expected = self.write_color_pattern()
        self.configure()
        self.request_frame()
        self.complete_frame()
        self.assert_screenshot(expected)
        self.panel_command(0x10)
        self.assert_screenshot(BLACK)
        q.write(CPU_FB, b'\x20\x40\x80' * (WIDTH * HEIGHT))
        self.request_frame()
        q.step(20_000_000)
        self.assertEqual(q.read32(LCDC + 0x1C4) & FRAME_DONE, 0)
        self.assertEqual(q.read32(LCDC + 0x224) & 1, 1)
        self.assertFalse(q.irq(27))
        self.assert_screenshot(BLACK)
        self.panel_command(0x11)
        self.assert_screenshot(expected)
        self.complete_frame()
        self.assert_screenshot(b'\x80\x40\x20' * (WIDTH * HEIGHT))
        self.panel_command(0x28)
        self.assert_screenshot(BLACK)
        q.write(CPU_FB, b'\x33\x66\x99' * (WIDTH * HEIGHT))
        self.request_frame()
        self.complete_frame()
        self.assertTrue(q.irq(27), 'awake display-off may still update hidden GRAM')
        self.assert_screenshot(BLACK)
        self.panel_command(0x29)
        self.assert_screenshot(b'\x99\x66\x33' * (WIDTH * HEIGHT))

    def test_dsi_software_reset_requires_new_transfer_to_restore_pixels(self):
        q = self.machine
        expected = self.write_color_pattern()
        self.configure()
        self.request_frame()
        self.complete_frame()
        self.assert_screenshot(expected)
        self.panel_command(0x01)
        self.panel_command(0x11)
        self.panel_command(0x29)
        q.step(20_000_000)
        self.assert_screenshot(BLACK)
        self.assertEqual(q.read32(LCDC + 0x224) & 1, 0)
        self.request_frame()
        self.complete_frame()
        self.assert_screenshot(expected)

    def test_unmapped_and_cross_alias_source_cannot_complete_or_replace_gram(self):
        q = self.machine
        expected = self.write_color_pattern()
        self.configure()
        self.request_frame()
        self.complete_frame()
        for address in (0x70000000, DSI, 0x213FFFF0):
            with self.subTest(address=hex(address)):
                self.configure(address)
                self.request_frame()
                q.step(20_000_000)
                self.assertEqual(q.read32(LCDC + 0x1C4) & FRAME_DONE, 0)
                self.assertEqual(q.read32(LCDC + 0x224) & 1, 1)
                self.assertFalse(q.irq(27))
                self.assert_screenshot(expected)


class Best1700LcdcSecureQtest(Best1700LcdcQtest):
    """The same frame contracts with real TGU/MPC address translations.

    qtest does not execute this synthetic secure image. It exists to select
    the secure memory topology without depending on any external firmware.
    Device DMA currently retains unspecified transaction attributes.
    """

    def machine_options(self):
        temp = tempfile.TemporaryDirectory(prefix='best1700-lcdc-secure-')
        self.addCleanup(temp.cleanup)
        tee = Path(temp.name) / 'tee.bin'
        tee.write_bytes(struct.pack('<II', 0x2415f000, 0x100c0009) + b'\xfe\xe7')
        return (f',secure-boot=on,tee-image={tee},tee-flash-base=0x10000000'
                ',tee-code-offset=0xc0000,tee-entry=0x100c0009'
                ',boot-sp=0x2415f000,boot-pc=0x100c0009')

    def test_tgu_hole_and_discontinuous_ram_preserve_completed_frame(self):
        q = self.machine
        expected = self.write_color_pattern()
        self.configure()
        self.request_frame()
        self.complete_frame()
        self.assert_screenshot(expected)
        # A valid first RAM segment must not authorize a later hole, a
        # discontinuous offset in the same RAM, or a different RAM region.
        for address in (0x21800ff0, 0x213ffff0, 0x23bffff0):
            with self.subTest(address=hex(address)):
                self.configure(address)
                self.request_frame()
                q.step(20_000_000)
                self.assertEqual(q.read32(LCDC + 0x1C4) & FRAME_DONE, 0)
                self.assertEqual(q.read32(LCDC + 0x224) & 1, 1)
                self.assertFalse(q.irq(27))
                self.assert_screenshot(expected)
        # Correcting the source can complete the pending transfer normally.
        self.configure()
        self.request_frame()
        self.complete_frame()
        self.assert_screenshot(expected)


if __name__ == '__main__':
    unittest.main()
