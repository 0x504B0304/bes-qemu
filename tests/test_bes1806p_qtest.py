"""PMU PWRKEY bus registers, native interrupt input and whole-machine reset."""
import os
import unittest
import time

from tests.test_best1700_peripherals_qtest import QtestMachine
from tests import test_best1700_cp_execution_qtest as cp_qtest

ISPI = 0x4000a000
PRESS, RELEASE, LEVEL = 1 << 10, 1 << 11, 1 << 9


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'), 'QEMU not configured')
class PmuQtest(unittest.TestCase):
    assemble = cp_qtest.CpExecutionQtest.assemble
    def setUp(self):
        self.q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'], extra_args=(
            '-device', 'bes1806p-pmu,id=pmu,bus=/best1700-ispi/ispi,'
            'chip-select=0,key-code=home'))
        self.addCleanup(self.q.close)
        self.q.command('irq_intercept_in /machine/core[0]')

    def read(self, reg):
        self.q.write32(ISPI + 4, 0)
        self.q.write32(ISPI + 8, (1 << 24) | (reg << 16))
        return self.q.read32(ISPI + 8)

    def write(self, reg, value):
        self.q.write32(ISPI + 4, 0)
        self.q.write32(ISPI + 8, (reg << 16) | value)
        self.q.read32(ISPI + 8)

    def key(self, down, name='home'):
        self.q.qmp('input-send-event', {'events': [{'type': 'key', 'data': {
            'down': down, 'key': {'type': 'qcode', 'data': name}}}]})

    def test_identity_fallback_and_chip_select_isolation(self):
        self.assertEqual([self.read(reg) for reg in range(3)], [0x18f0, 0x650, 0])
        self.write(1, 0xffff)
        self.assertEqual(self.read(1), 0x650)
        self.write(0x42, 0x1234)
        self.assertEqual(self.read(0x42), 0x1234)
        self.q.write32(ISPI + 4, 7 << 7)
        self.q.write32(ISPI + 8, (1 << 28))
        self.assertEqual(self.q.read32(ISPI + 8), 0x1700 << 2)
        self.assertEqual(self.read(0), 0x18f0)

    def test_normal_frame_and_full_register_address_do_not_alias(self):
        q = self.q
        q.write32(ISPI, (28 << 16) | 0x100)
        self.assertEqual(q.read32(ISPI), (28 << 16) | 0x100)
        def read_normal(reg):
            q.write32(ISPI + 4, 0)
            q.write32(ISPI + 8, (1 << 28) | (reg << 18))
            return q.read32(ISPI + 8) >> 2
        def write_normal(reg, value):
            q.write32(ISPI + 4, 0)
            q.write32(ISPI + 8, (reg << 18) | (value << 2))
            q.read32(ISPI + 8)
        self.assertEqual(read_normal(0), 0x18f0)
        write_normal(0x142, 0x2468)
        write_normal(0x42, 0x1357)
        self.assertEqual(read_normal(0x142), 0x2468)
        self.assertEqual(read_normal(0x42), 0x1357)
        write_normal(0x44, 8)
        self.key(True)
        self.assertEqual(read_normal(0x5e), PRESS | LEVEL)
        self.assertTrue(q.irq(23))
        write_normal(0x5e, PRESS)
        self.assertFalse(q.irq(23))
        q.write32(ISPI, 24 << 16)
        self.assertEqual(self.read(0x44), 8)
        self.assertEqual(self.read(0x5e), LEVEL)

    def test_gpadc_start_status_clear_and_interval_resampling(self):
        self.write(0x27, 1)
        self.assertEqual(self.read(0x52), 0)
        self.write(0x4f, 0x20)
        self.assertEqual(self.read(0x52), 1)
        self.assertEqual(self.read(0x56), 0x6e00)
        self.write(0x52, 1)
        self.assertEqual(self.read(0x52), 1)
        self.write(0x4f, 0)
        self.write(0x52, 1)
        self.assertEqual(self.read(0x52), 0)

    def test_gpio_and_pmu_buttons_share_keyboard_without_stealing_events(self):
        # Test both device realization orders, including simultaneous holds.
        options = ['bes1806p-pmu,bus=/best1700-ispi/ispi,key-code=home',
                   'miband-gpio-button,target=/machine/gpio,pin=76,key-code=end']
        for order in (options, options[::-1]):
            q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                             extra_args=tuple(arg for opt in order for arg in ('-device', opt)))
            self.addCleanup(q.close)
            def key(name, down):
                q.qmp('input-send-event', {'events': [{'type': 'key', 'data': {
                    'down': down, 'key': {'type': 'qcode', 'data': name}}}]})
            def status():
                q.write32(ISPI + 4, 0)
                q.write32(ISPI + 8, (1 << 24) | (0x5e << 16))
                return q.read32(ISPI + 8)
            gpio, mask = 0x4008a050, 1 << 12
            key('home', True)
            self.assertEqual(status(), PRESS | LEVEL)
            self.assertEqual(q.read32(gpio) & mask, mask)
            key('end', True)
            self.assertEqual(status(), PRESS | LEVEL)
            self.assertEqual(q.read32(gpio) & mask, 0)
            key('home', False)
            self.assertEqual(status(), PRESS | RELEASE)
            self.assertEqual(q.read32(gpio) & mask, 0)
            key('end', False)
            self.assertEqual(q.read32(gpio) & mask, mask)

    def test_press_release_latch_mask_and_w1c(self):
        self.key(True, 'end')
        self.assertEqual(self.read(0x5e), 0)
        self.key(False, 'end')
        self.key(True)
        self.assertEqual(self.read(0x5e), PRESS | LEVEL)
        self.assertFalse(self.q.irq(23))
        self.write(0x44, 8)
        self.assertTrue(self.q.irq(23))
        self.write(0x5e, LEVEL)  # Physical status is not guest-writable.
        self.assertEqual(self.read(0x5e), PRESS | LEVEL)
        self.write(0x5e, PRESS)
        self.assertEqual(self.read(0x5e), LEVEL)
        self.assertFalse(self.q.irq(23))
        self.key(True)  # Autorepeat must not produce another press edge.
        self.assertEqual(self.read(0x5e), LEVEL)
        self.key(False)
        self.assertEqual(self.read(0x5e), RELEASE)
        self.assertFalse(self.q.irq(23))
        self.write(0x44, 16)
        self.assertTrue(self.q.irq(23))
        self.write(0x44, 0)
        self.assertFalse(self.q.irq(23))
        self.write(0x5e, RELEASE)
        self.write(0x44, 24)
        self.assertFalse(self.q.irq(23))

    def test_both_pending_edges_and_two_resets_keep_endpoint_attached(self):
        for _ in range(2):
            self.write(0x44, 24)
            self.key(True)
            self.key(False)
            self.assertEqual(self.read(0x5e), PRESS | RELEASE)
            self.write(0x5e, PRESS)
            self.assertTrue(self.q.irq(23))
            self.write(0x5e, RELEASE)

            self.assertFalse(self.q.irq(23))
            self.key(True)
            self.q.qmp('system_reset')
            self.q.qmp('query-status')
            self.assertEqual(self.read(0x5e), 0)
            self.assertEqual(self.read(0x44), 0)
            self.assertEqual(self.read(0), 0x18f0)
            self.assertFalse(self.q.irq(23))
            self.key(False)
            self.write(0x5e, RELEASE)

    def test_mttcg_native_m55_isr_consumes_press_and_release_after_two_boots(self):
        payload = self.assemble(r'''
.syntax unified
.arch armv8-m.main
.thumb
.section .text
.global reset_a
vectors:
    .word 0x203ff000
    .word reset_a
    .rept 37
    .word unexpected
    .endr
    .word pmu_irq
    .rept 104
    .word unexpected
    .endr
.org 0x400
.thumb_func
reset_a:
    ldr r4, =0x20001000
    movs r0, #0
    str r0, [r4]
    str r0, [r4, #4]
    ldr r0, =vectors
    ldr r1, =0xe000ed08
    str r0, [r1]
    ldr r1, =0x4000a000
    ldr r0, =0x001c0100
    str r0, [r1]
    movs r0, #0
    str r0, [r1, #4]
    ldr r0, =(0x44 << 18) | (24 << 2)
    str r0, [r1, #8]
    ldr r0, [r1, #8]
    ldr r0, =0x00800000
    ldr r1, =0xe000e100
    str r0, [r1]
    movs r0, #1
    str r0, [r4]
    cpsie i
    dsb sy
1:  wfi
    b 1b
.thumb_func
pmu_irq:
    push {r0-r3, lr}
    ldr r1, =0x4000a000
    ldr r2, =(1 << 28) | (0x5e << 18)
    str r2, [r1, #8]
    ldr r2, [r1, #8]
    lsrs r2, #2
    ldr r0, =0x20001000
    str r2, [r0, #8]
    and r2, r2, #0xc00
    lsls r2, #2
    ldr r3, =(0x5e << 18)
    orrs r2, r3
    str r2, [r1, #8]
    ldr r2, [r1, #8]
    ldr r2, [r0, #4]
    adds r2, #1
    str r2, [r0, #4]
    mrs r2, IPSR
    str r2, [r0, #12]
    pop {r0-r3, pc}
.thumb_func
unexpected:
    ldr r0, =0x20001000
    movs r1, #255
    str r1, [r0]
2:  b 2b
    .ltorg
''', 0x101c0000)
        q = QtestMachine(os.environ['MIBAND_BEST1700_QEMU'], image_payload=payload,
            accel='tcg,thread=multi', machine_options=',boot-sp=0x203ff000,boot-pc=0x101c0401',
            extra_args=('-S', '-device', 'bes1806p-pmu,bus=/best1700-ispi/ispi,key-code=home'))
        self.addCleanup(q.close)
        def await_word(address, value):
            deadline = time.monotonic() + 3
            while q.read32(address) != value:
                if time.monotonic() > deadline:
                    self.fail('native PMU IRQ23 did not complete: ' + q.diagnostic('PMU'))
                time.sleep(.005)
        for boot in range(2):
            if boot:
                q.qmp('system_reset')
                q.write(0x20001000, bytes(16))
            q.qmp('cont')
            await_word(0x20001000, 1)
            for count, down, expected in [(1, True, PRESS | LEVEL), (2, False, RELEASE)]:
                q.qmp('input-send-event', {'events': [{'type': 'key', 'data': {
                    'down': down, 'key': {'type': 'qcode', 'data': 'home'}}}]})
                await_word(0x20001004, count)
                self.assertEqual(q.read32(0x20001008), expected)
                self.assertEqual(q.read32(0x2000100c), 16 + 23)
                self.assertEqual(q.read32(0x20001000), 1)
            q.qmp('stop')


if __name__ == '__main__':
    unittest.main()
