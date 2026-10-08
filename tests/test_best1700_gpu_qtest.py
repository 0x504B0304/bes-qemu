"""GC NanoUltraV identity, real DMA rendering, masking and failure contracts."""
import os
import struct
import unittest

from tests.test_best1700_peripherals_qtest import QtestMachine
from tests import test_best1700_mpc_qtest as mpc
from tests.test_gpu_bgr24_texture_qtest import TARGET, fp, pattern, state, path_rectangle

GPU = 0x401F0000
COMMAND = 0x18100000
SOURCE = 0x18200000
DESTINATION = 0x20160000


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700GpuQtest(unittest.TestCase):
    def setUp(self):
        self.machine = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'])
        self.addCleanup(self.machine.close)
        self.machine.command('irq_intercept_in /machine/core[0]')

    def submit(self, commands, *, address=COMMAND, mask=1):
        q = self.machine
        q.write(address, commands)
        q.write32(GPU + 0x14, mask)
        q.write32(GPU + 0x500, address)
        q.write32(GPU + 0x504, len(commands) // 8)

    def test_identity_reset_and_idle(self):
        q = self.machine
        for offset, expected in ((0x20, 0x265), (0x24, 0x2002),
                                 (0x30, 0x420), (0xE8, 0)):
            self.assertEqual(q.read32(GPU + offset), expected)
        self.assertEqual(q.read32(GPU + 4), 0xB05)
        q.write32(GPU, 1 << 12)
        self.assertEqual(q.read32(GPU + 4), 0)
        q.write32(GPU, 0)
        self.assertEqual(q.read32(GPU + 4), 0xB05)

    def test_scaled_texture_writes_tcm_through_axi_alias(self):
        q = self.machine
        # A 2x2 POINT texture enlarged to 4x4 has an independent quadrant
        # oracle. Submit through the noncached PSRAM alias, target AXI TCM.
        colors = (b'\x13\x25\x37', b'\x41\x53\x65',
                  b'\x79\x8b\x9d', b'\xa7\xb9\xcb')
        q.write(SOURCE, b''.join(colors))
        q.write(DESTINATION - 16, b'\xd3' * (16 + 48 + 16))
        commands = pattern(4, 4, SOURCE, source_width=2, source_height=2,
                           register_stride=8, offset_x=0.25, offset_y=0.25)
        commands = commands.replace(state(0xA11, TARGET),
                                    state(0xA11, 0x21160000))
        commands = (commands[:-64] + state(0xA1C, fp(0.25)) +
                    state(0xA21, fp(0.25)) + commands[-64:] + bytes(8))
        self.submit(commands, address=0x38100000, mask=0)
        expected = ((colors[0] * 2 + colors[1] * 2) * 2 +
                    (colors[2] * 2 + colors[3] * 2) * 2)
        self.assertEqual(q.read(DESTINATION, 48), expected)
        self.assertEqual(q.read(DESTINATION - 16, 16), b'\xd3' * 16)
        self.assertEqual(q.read(DESTINATION + 48, 16), b'\xd3' * 16)
        self.assertFalse(q.irq(33))
        q.write32(GPU + 0x14, 1)
        self.assertTrue(q.irq(33), 'unmask must expose retained completion')
        self.assertEqual(q.read32(GPU + 0x10), 1)
        self.assertFalse(q.irq(33), 'read-to-clear must lower GPU IRQ33')

    def test_rectangle_fifo_modes_preserve_premultiplied_alpha(self):
        q = self.machine
        # The FIFO workaround bit cannot change texture or blending pixels.
        q.write(SOURCE, bytes((20, 40, 60, 128)))
        for mode in (0x10001101, 0x10001181):
            with self.subTest(mode=hex(mode)):
                q.write(DESTINATION, bytes((100, 80, 60, 200)))
                commands = pattern(1, 1, SOURCE, source_format=0x01000107)
                commands = commands[:-64]
                commands += b''.join(state(r, v) for r, v in (
                    (0xA10, 0x103), (0xA11, DESTINATION), (0xA00, mode)))
                commands += struct.pack('<II4H', 0x40000001, 0, 0, 0, 1, 1)
                commands += bytes(8)
                self.submit(commands)
                self.assertEqual(q.read(DESTINATION, 4), bytes((70, 80, 90, 228)))
                self.assertEqual(q.read32(GPU + 0x10), 1)

    def test_rectangle_multiply_modulates_rgb_and_alpha(self):
        q = self.machine
        q.write(SOURCE, bytes((40, 80, 120, 128)))
        for mode in (0x10002101, 0x10002181):
            with self.subTest(mode=hex(mode)):
                q.write(DESTINATION, bytes((100, 80, 60, 200)))
                commands = pattern(1, 1, SOURCE, source_format=0x01000107)[:-64]
                commands += b''.join(state(r, v) for r, v in (
                    (0xA10, 0x103), (0xA11, DESTINATION), (0xA00, mode),
                    (0xA02, 0x80808080)))
                commands += struct.pack('<II4H', 0x40000001, 0, 0, 0, 1, 1) + bytes(8)
                self.submit(commands)
                # Modulated premultiplied source is (20,40,60,64), then
                # SRC_OVER retains 191/255 of the destination channels.
                self.assertEqual(q.read(DESTINATION, 4), bytes((95, 100, 105, 214)))
                self.assertEqual(q.read32(GPU + 0x10), 1)

    def test_a8_path_alpha_and_byte_stride(self):
        q = self.machine
        q.write(DESTINATION - 8, b'\xd3' * 8 + bytes((200,)) * 16 + b'\xd3' * 8)
        # A half-opacity solid path covers only the centre 2x2. A8 must use
        # one byte per pixel and retain all pixels outside the tessellation.
        commands = b''.join(state(r, v) for r, v in (
            (0xA10, 0x106), (0xA11, DESTINATION), (0xA12, 4),
            (0xA13, 4 | (4 << 16)), (0xA00, 0x10000102),
            (0xA02, 0x80402010), (0xA34, 0x03300010),
            (0xA40, fp(1)), (0xA41, 0), (0xA42, 0),
            (0xA43, 0), (0xA44, fp(1)), (0xA45, 0),
            (0xA3B, fp(1)), (0xA3C, 0),
            (0xA39, 0), (0xA3A, 4 | (4 << 16)), (0xAD1, 0)))
        commands += path_rectangle(1, 1, 3, 3) + bytes(8)
        self.submit(commands)
        self.assertEqual(q.read32(GPU + 0x10), 1)
        self.assertEqual(q.read(DESTINATION, 16), bytes((
            200, 200, 200, 200, 200, 228, 228, 200,
            200, 228, 228, 200, 200, 200, 200, 200)))
        self.assertEqual(q.read(DESTINATION - 8, 8), b'\xd3' * 8)
        self.assertEqual(q.read(DESTINATION + 16, 8), b'\xd3' * 8)

    def gaussian(self, weights):
        commands = pattern(3, 3, SOURCE, source_format=0x21030100)[:-64]
        commands += b''.join(state(r, v) for r, v in (
            (0xA11, DESTINATION), (0xA00, 0x10001001),
            (0xAD3, weights[0]), (0xAD5, weights[1]), (0xAD7, weights[2])))
        return commands + struct.pack('<II4H', 0x40000001, 0, 0, 0, 3, 3) + bytes(8)

    def test_gaussian_impulse_weights_and_kernel_update(self):
        q = self.machine
        source = bytes(12) + bytes((80, 160, 240)) + bytes(12)
        q.write(SOURCE, source)
        q.write(DESTINATION - 8, b'\xd3' * (8 + 27 + 8))
        self.submit(self.gaussian((64, 32, 16)))
        # One centre impulse produces three distinct neighbourhood classes.
        corner, axial, centre = bytes((5, 10, 15)), bytes((10, 20, 30)), bytes((20, 40, 60))
        self.assertEqual(q.read(DESTINATION, 27),
                         corner + axial + corner + axial + centre + axial + corner + axial + corner)
        self.assertEqual(q.read32(GPU + 0x10), 1)
        self.assertEqual(q.read(DESTINATION - 8, 8), b'\xd3' * 8)
        self.assertEqual(q.read(DESTINATION + 27, 8), b'\xd3' * 8)
        self.submit(self.gaussian((256, 0, 0)))
        self.assertEqual(q.read(DESTINATION, 27), source)
        self.assertEqual(q.read32(GPU + 0x10), 1)

    def test_gaussian_color_border_and_quantized_sum(self):
        q = self.machine
        q.write(SOURCE, bytes((128, 64, 32)) * 9)
        self.submit(self.gaussian((64, 32, 16)))
        # Transparent COLOR taps outside the 3x3 source: corner/edge/interior
        # sums are 144/192/256. Destination BGR discards filtered alpha.
        corner, edge, centre = bytes((72, 36, 18)), bytes((96, 48, 24)), bytes((128, 64, 32))
        self.assertEqual(q.read(DESTINATION, 27),
                         corner + edge + corner + edge + centre + edge + corner + edge + corner)
        self.assertEqual(q.read32(GPU + 0x10), 1)
        q.write(SOURCE, bytes((255,)) * 27)
        self.submit(self.gaussian((30, 29, 27)))
        self.assertEqual(q.read(DESTINATION + 12, 3), bytes((253,)) * 3,
                         '254/256 quantized kernel must not be renormalized')
        self.assertEqual(q.read32(GPU + 0x10), 1)

    def test_gaussian_invalid_coefficients_withhold_completion(self):
        q = self.machine
        q.write(SOURCE, bytes((80, 160, 240)) * 9)
        for weights in ((0, 0, 0), (257, 0, 0), (256, 1, 0), (0xffffffff, 0, 0)):
            with self.subTest(weights=weights):
                q.write(DESTINATION, b'\xd3' * 27)
                self.submit(self.gaussian(weights))
                self.assertEqual(q.read(DESTINATION, 27), b'\xd3' * 27)
                self.assertEqual(q.read32(GPU + 0x10), 0)
                self.assertFalse(q.irq(33))

    def test_rectangle_none_replaces_translucent_destination(self):
        q = self.machine
        q.write(SOURCE, bytes((20, 40, 60, 128)))
        for mode in (0x10001001, 0x10001081):
            with self.subTest(mode=hex(mode)):
                q.write(DESTINATION, bytes((100, 80, 60, 200)))
                commands = pattern(1, 1, SOURCE, source_format=0x01000107)[:-64]
                commands += b''.join(state(r, v) for r, v in (
                    (0xA10, 0x103), (0xA11, DESTINATION), (0xA00, mode)))
                commands += struct.pack('<II4H', 0x40000001, 0, 0, 0, 1, 1) + bytes(8)
                self.submit(commands)
                self.assertEqual(q.read(DESTINATION, 4), bytes((20, 40, 60, 128)))
                self.assertEqual(q.read32(GPU + 0x10), 1)

    def test_unsupported_stream_and_invalid_dma_do_not_complete(self):
        q = self.machine
        self.submit(struct.pack('<II', 0xF0000000, 0))
        self.assertEqual(q.read32(GPU + 0x10), 0)
        self.assertFalse(q.irq(33))
        q.write32(GPU + 0x500, 0x401F0000)  # MMIO is not command RAM.
        q.write32(GPU + 0x504, 1)
        self.assertEqual(q.read32(GPU + 0x10), 0)
        self.assertFalse(q.irq(33))
        self.submit(bytes(8))
        self.assertTrue(q.irq(33))
        q.write32(GPU, 1 << 12)
        self.assertEqual(q.read32(GPU + 0x10), 0)
        self.assertFalse(q.irq(33))


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700GpuSecurityQtest(unittest.TestCase):
    assemble = mpc.Best1700MpcQtest.assemble
    run_guest = mpc.Best1700MpcQtest.run_guest

    def test_sram_command_fetch_uses_gpu_master_policy(self):
        # Real Secure M55 writes a minimal END stream in SRAM bank 5 and
        # changes its MPC attribution independently of the GPU bus master.
        body = """
    ldr r1, =0x22500000
    movs r0, #0
    str r0, [r1]
    str r0, [r1, #4]
""" + mpc.lut_word(0x44064000, 0, 0xffffffff)
        for index, policy in enumerate((0, 64)):
            body += f"""
    ldr r5, =0x44088048
    movs r0, #{policy}
    str r0, [r5]
    ldr r6, ={GPU:#x}
    ldr r0, =0x22500000
    str r0, [r6, #0x500]
    movs r0, #1
    str r0, [r6, #0x504]
    ldr r0, [r6, #0x10]
    str r0, [r4, #{4 + index * 4}]
"""
        body += mpc.lut_word(0x44064000, 0, 0)
        for index, policy in enumerate((64, 0), start=2):
            body += f"""
    ldr r5, =0x44088048
    movs r0, #{policy}
    str r0, [r5]
    ldr r6, ={GPU:#x}
    movs r0, #1
    str r0, [r6, #0x504]
    ldr r0, [r6, #0x10]
    str r0, [r4, #{4 + index * 4}]
"""
        values = self.run_guest(body, cycles=2)
        for result in values:
            self.assertEqual(result[1:5], (0, 1, 0, 1))


if __name__ == '__main__':
    unittest.main()
