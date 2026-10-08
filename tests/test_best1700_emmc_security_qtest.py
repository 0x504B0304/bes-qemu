"""Exercise eMMC bus-master security with real Secure M55 transactions.

The guest configures SAU/MPC/TGU permissions and copies the DMA destination
to secure DTCM. qtest never reads nonsecure PSRAM with unspecified attributes.
Each case owns its synthetic images, raw disk, QEMU process and QMP socket.
"""

import os
from pathlib import Path
import struct
import tempfile
import time
import unittest

from tests import test_best1700_emmc_qtest as emmc
from tests import test_best1700_mpc_qtest as mpc
from tests.test_best1700_peripherals_qtest import QtestMachine


RESULT = 0x20001000
RESULT_SECURE = 0x24001000
PAYLOAD = 0x18008000
MASTER = 0x44088044
READY = 0xead1
DONE = 0x600d
FAULT = 0xfa17
COPY_OFFSET = 0x40
SENTINEL = bytes([0xa5]) * emmc.BLOCK


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class Best1700EmmcSecurityQtest(unittest.TestCase):
    # Reuse the existing protocol/assembly helpers without inheriting their
    # unrelated test methods or starting an additional unprotected machine.
    assemble = mpc.Best1700MpcQtest.assemble
    write8 = emmc.Best1700EmmcQtest.write8
    write16 = emmc.Best1700EmmcQtest.write16
    read16 = emmc.Best1700EmmcQtest.read16
    configure_host = emmc.Best1700EmmcQtest.configure_host
    command = emmc.Best1700EmmcQtest.command
    initialize_card = emmc.Best1700EmmcQtest.initialize_card

    def guest_source(self, *, master_ns, target_ns, ns_write=False,
                     register_probe=False, reset_master=False):
        setup = f"""
    ldr r1, ={PAYLOAD:#x}
    ldr r0, =0xa5a5a5a5
    movs r2, #{emmc.BLOCK // 4}
fill_payload:
    str r0, [r1], #4
    subs r2, #1
    bne fill_payload
    ldr r1, ={emmc.CPU_DESCRIPTORS:#x}
    ldr r0, =0x02000023
    str r0, [r1]
    ldr r0, ={PAYLOAD:#x}
    str r0, [r1, #4]
    ldr r5, ={MASTER:#x}
    ldr r0, [r5]
    str r0, [r4, #36]
"""
        if reset_master:
            # The first boot establishes NS ownership. After system_reset,
            # the host sets a marker outside the guest's cleared result area;
            # the second boot must use reset ownership without rewriting it.
            setup += f"""
    ldr r1, ={RESULT_SECURE + 0x300:#x}
    ldr r0, [r1]
    cmp r0, #0
    bne master_ready
    movs r0, #4
    str r0, [r5]
master_ready:
"""
        else:
            setup += f"""
    movs r0, #{4 if master_ns else 0}
    str r0, [r5]
"""
        if register_probe:
            setup += f"""
    movs r0, #4
    str r0, [r5]
    movs r0, #255
    strb r0, [r5, #1]
    ldr r0, [r5]
    str r0, [r4, #40]
    ldr r0, =0xffff
    strh r0, [r5, #2]
    ldr r0, [r5]
    str r0, [r4, #44]
    movs r0, #2
    strb r0, [r5]
    ldr r0, [r5]
    str r0, [r4, #48]
    movs r0, #6
    strh r0, [r5]
    ldr r0, [r5]
    str r0, [r4, #52]
    movs r0, #{4 if master_ns else 0}
    str r0, [r5]
"""
        setup += 'dsb sy\n'
        if master_ns:
            # The descriptor's AXI and core views share the same DTCM gate.
            # Derive its granule and LUT word from DTGU_CFG, not page guesses.
            setup += f"""
    ldr r5, =0xe001e000
    ldr r0, [r5, #0x604]
    and r0, r0, #31
    adds r0, #5
    ldr r1, ={emmc.CPU_DESCRIPTORS - 0x20000000:#x}
    lsrs r1, r1, r0
    and r2, r1, #31
    movs r0, #1
    lsls r0, r0, r2
    lsrs r1, r1, #5
    lsls r1, r1, #2
    addw r5, r5, #0x610
    str r0, [r5, r1]
"""
        setup += mpc.lut_word(mpc.PSRAM_MPC, PAYLOAD - 0x18000000,
                              0xffffffff if target_ns else 0)
        if ns_write:
            setup += mpc.ns_setup([0x40088044]) + mpc.ns_call()
            # A successful return makes the attempted NS register write an
            # actual CPU access, rather than a qtest access with debug attrs.
            setup += 'movs r0, #1\nstr r0, [r4, #24]\n'
        regions = [(PAYLOAD, PAYLOAD + emmc.BLOCK - 1, False)] if target_ns else []
        setup += mpc.sau_regions(regions)
        return r"""
.syntax unified
.thumb
.section .text
.word 0x2415f000
.word reset
.rept 142
.word fault
.endr
.org 0x400
.thumb_func
reset:
    cpsid i
    ldr r4, =0x24001000
    mov r1, r4
    movs r0, #0
    movs r2, #160
clear_result:
    str r0, [r1], #4
    subs r2, #1
    bne clear_result
    ldr r0, =0x100c0000
    ldr r1, =0xe000ed08
    str r0, [r1]
""" + setup + f"""
    ldr r0, ={READY:#x}
    str r0, [r4]
wait_release:
    ldr r0, [r4, #4]
    cmp r0, #0
    beq wait_release
    ldr r5, ={mpc.PSRAM_MPC:#x}
    ldr r0, [r5, #0x20]
    str r0, [r4, #8]
    ldr r0, [r5, #0x2c]
    str r0, [r4, #12]
    ldr r0, [r5, #0x30]
    str r0, [r4, #16]
    ldr r5, ={MASTER:#x}
    ldr r0, [r5]
    str r0, [r4, #20]
    ldr r1, ={PAYLOAD:#x}
    add r2, r4, #{COPY_OFFSET}
    movs r3, #{emmc.BLOCK // 4}
copy_payload:
    ldr r0, [r1], #4
    str r0, [r2], #4
    subs r3, #1
    bne copy_payload
    dsb sy
    ldr r0, ={DONE:#x}
    str r0, [r4]
halt:
    b halt
.thumb_func
fault:
    ldr r4, ={RESULT_SECURE:#x}
    mrs r0, IPSR
    str r0, [r4, #28]
    ldr r0, =0xe000ed28
    ldr r0, [r0]
    str r0, [r4, #32]
    ldr r0, ={FAULT:#x}
    str r0, [r4]
    b halt
.ltorg
"""

    def wait_guest(self, expected):
        q = self.machine
        q.qmp('cont')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if q.read32(RESULT) in (expected, FAULT):
                break
            time.sleep(0.01)
        q.qmp('stop')
        state = q.read32(RESULT)
        self.assertEqual(state, expected,
                         f'guest state {state:#x}, result={q.read(RESULT, COPY_OFFSET).hex()}\n'
                         + q.diagnostic('eMMC security guest'))

    def run_read(self, *, adma, master_ns, target_ns, ns_write=False,
                 register_probe=False, reset_master=False):
        with tempfile.TemporaryDirectory(prefix='best1700-emmc-security-') as temp:
            directory = Path(temp)
            disk = directory / 'card.raw'
            with disk.open('wb') as stream:
                stream.truncate(emmc.DISK_BYTES)
                stream.seek(emmc.SEED_BLOCK * emmc.BLOCK)
                stream.write(emmc.SEED)
            tee = self.assemble(directory, 'tee', self.guest_source(
                master_ns=master_ns, target_ns=target_ns, ns_write=ns_write,
                register_probe=register_probe, reset_master=reset_master), 0x100c0000)
            ap = None
            if ns_write:
                nonsecure = f"""
.syntax unified
.thumb
.section .text
.word 0x203ff000
.word first
.rept 142
.word failure
.endr
.org 0x400
.thumb_func
first:
    ldr r3, =0x40088044
    movs r0, #{0 if master_ns else 4}
    str r0, [r3]
    dsb sy
    bx lr
.thumb_func
failure:
    b failure
.ltorg
"""
                ap = self.assemble(directory, 'ap', nonsecure, 0x101c0000).read_bytes()
            self.machine = QtestMachine(
                os.environ['MIBAND_BEST1700_QEMU'], image_payload=ap, accel='tcg',
                machine_options=(',secure-boot=on,boot-sp=0x2415f000,boot-pc=0x100c0401'
                                 f',tee-image={tee},tee-flash-base=0x10000000'
                                 ',tee-code-offset=0xc0000,tee-entry=0x100c0401'
                                 ',emmc-drive=emmc-security'),
                extra_args=('-S', '-icount', 'shift=0,sleep=off', '-drive',
                            f'if=none,id=emmc-security,file={disk},format=raw'))
            q = self.machine
            try:
                self.wait_guest(READY)
                if reset_master:
                    q.qmp('system_reset')
                    q.qmp('query-status')
                    q.write32(RESULT, 0)
                    q.write32(RESULT + 0x300, 1)
                    self.wait_guest(READY)
                self.initialize_card(signal=False)
                self.command(23, 1)
                self.write8(0x28, 0x10 if adma else 0)
                q.write32(emmc.HOST + 0x04, emmc.BLOCK)
                if adma:
                    q.write32(emmc.HOST + 0x58, emmc.AXI_DESCRIPTORS)
                    q.write32(emmc.HOST + 0x5c, 0)
                else:
                    q.write32(emmc.HOST, PAYLOAD)
                q.write32(emmc.HOST + 0x04, (1 << 16) | emmc.BLOCK)
                q.write32(emmc.HOST + 0x30, 0xffffffff)
                argument = emmc.SEED_BLOCK if self.block_addressed else emmc.SEED_BLOCK * emmc.BLOCK
                q.write32(emmc.HOST + 0x08, argument)
                q.write32(emmc.HOST + 0x0c, (17 << 24) | ((emmc.R1 | 0x20) << 16) | 0x13)
                status = q.read32(emmc.HOST + 0x30)
                q.write32(RESULT + 4, 1)
                self.wait_guest(DONE)
                values = struct.unpack('<16I', q.read(RESULT, COPY_OFFSET))
                payload = q.read(RESULT + COPY_OFFSET, emmc.BLOCK)
                return status, values, payload
            finally:
                q.close()

    def test_dma_master_and_psram_permissions(self):
        for adma in (False, True):
            for master_ns in (False, True):
                for target_ns in (False, True):
                    with self.subTest(adma=adma, master_ns=master_ns, target_ns=target_ns):
                        status, values, payload = self.run_read(
                            adma=adma, master_ns=master_ns, target_ns=target_ns)
                        self.assertEqual(values[5] & 4, 4 if master_ns else 0)
                        if master_ns == target_ns:
                            self.assertEqual(status, emmc.COMMAND_DONE | emmc.TRANSFER_DONE)
                            self.assertEqual(payload, emmc.SEED[:emmc.BLOCK],
                                             'allowed DMA must reach the PSRAM backing store')
                            self.assertEqual(values[2], 0, 'allowed DMA must not trip the MPC')
                        else:
                            self.assertEqual(payload, SENTINEL,
                                             'denied DMA must preserve the protected PSRAM bytes')

    def test_nonsecure_cpu_cannot_change_dma_master_security(self):
        for master_ns in (False, True):
            with self.subTest(master_ns=master_ns):
                status, values, payload = self.run_read(
                    adma=False, master_ns=master_ns, target_ns=master_ns, ns_write=True)
                self.assertEqual(values[6], 1, 'the Nonsecure register write must execute')
                self.assertEqual(values[5] & 4, 4 if master_ns else 0,
                                 'Nonsecure software must not change the DMA master security')
                self.assertEqual(status, emmc.COMMAND_DONE | emmc.TRANSFER_DONE)
                self.assertEqual(payload, emmc.SEED[:emmc.BLOCK])

    def test_subword_master_register_writes_preserve_other_bytes(self):
        status, values, payload = self.run_read(
            adma=False, master_ns=True, target_ns=True, register_probe=True)
        self.assertEqual(values[10:14], (4, 4, 2, 6),
                         'byte and halfword accesses must merge only their addressed lanes')
        self.assertEqual(status, emmc.COMMAND_DONE | emmc.TRANSFER_DONE)
        self.assertEqual(payload, emmc.SEED[:emmc.BLOCK])

    def test_system_reset_restores_secure_dma_master(self):
        status, values, payload = self.run_read(
            adma=False, master_ns=False, target_ns=False, reset_master=True)
        self.assertEqual(values[9], 0, 'system_reset must clear the previous NS master bit')
        self.assertEqual(values[5], 0, 'the guest must leave reset ownership unchanged')
        self.assertEqual(status, emmc.COMMAND_DONE | emmc.TRANSFER_DONE)
        self.assertEqual(payload, emmc.SEED[:emmc.BLOCK],
                         'reset must restore Secure transactions to Secure PSRAM')


if __name__ == '__main__':
    unittest.main()
