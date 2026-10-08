"""Media notifications must keep SD/eMMC transfer availability coherent."""
import os
from pathlib import Path
import unittest
from tests import test_best1700_emmc_qtest as base
from tests import test_sd_read_ahead as cached

@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'), 'built QEMU required')
class SdMediaState(unittest.TestCase):
    def test_remove_replace_reset_and_persistent_writes(self):
        for cache in (False, True):
            with self.subTest(read_ahead=cache):
                probe = cached.CachedEmmc() if cache else base.Best1700EmmcQtest()
                probe.setUp()
                try:
                    probe.initialize_card()
                    q = probe.machine
                    probe.transfer(base.SEED_BLOCK)
                    self.assertEqual(q.read(base.CPU_BUFFER, base.BLOCK), base.SEED[:base.BLOCK])
                    device = next(b['qdev'] for b in q.qmp('query-block') if b['device'] == 'emmc-test')
                    # Remove while a native PIO read has a partially consumed block.
                    argument = base.SEED_BLOCK if probe.block_addressed else base.SEED_BLOCK * base.BLOCK
                    q.write32(base.HOST + 4, (1 << 16) | base.BLOCK)
                    probe.command(17, argument, data=True, transfer=0x12)
                    q.read32(base.HOST + 0x20)
                    q.qmp('blockdev-remove-medium', {'id': device})
                    self.assertFalse(q.read32(base.HOST + 0x24) & 0x10000)
                    q.qmp('system_reset')
                    self.assertFalse(q.read32(base.HOST + 0x24) & 0x10000)
                    # No card must not complete a command successfully.
                    q.write32(base.HOST + 0x30, 0xffffffff)
                    q.write32(base.HOST + 0x0c, (13 << 24) | (base.R1 << 16))
                    self.assertFalse(q.read32(base.HOST + 0x30) & base.COMMAND_DONE)
                    replacement = Path(probe.temp.name) / 'replacement.raw'
                    payload = bytes((i * 19 + 71) & 255 for i in range(base.BLOCK))
                    with replacement.open('wb') as disk:
                        disk.truncate(base.DISK_BYTES)
                        disk.seek(base.SEED_BLOCK * base.BLOCK)
                        disk.write(payload)
                    q.qmp('blockdev-add', {'driver': 'raw', 'node-name': 'replacement',
                        'file': {'driver': 'file', 'filename': str(replacement)}})
                    q.qmp('blockdev-insert-medium', {'id': device, 'node-name': 'replacement'})
                    self.assertTrue(q.read32(base.HOST + 0x24) & 0x10000)
                    probe.initialize_card()
                    probe.transfer(base.SEED_BLOCK)
                    self.assertEqual(q.read(base.CPU_BUFFER, base.BLOCK), payload)
                    changed = bytes(b ^ 0xff for b in payload)
                    q.write(base.CPU_BUFFER, changed)
                    probe.transfer(base.SEED_BLOCK, write=True)
                    q.qmp('system_reset')
                    probe.initialize_card()
                    probe.transfer(base.SEED_BLOCK)
                    self.assertEqual(q.read(base.CPU_BUFFER, base.BLOCK), changed)
                    q.qmp('blockdev-remove-medium', {'id': device})
                    with replacement.open('rb') as disk:
                        disk.seek(base.SEED_BLOCK * base.BLOCK)
                        self.assertEqual(disk.read(base.BLOCK), changed)
                    with probe.disk.open('rb') as disk:
                        disk.seek(base.SEED_BLOCK * base.BLOCK)
                        self.assertEqual(disk.read(base.BLOCK), base.SEED[:base.BLOCK])
                finally:
                    probe.doCleanups()

if __name__ == '__main__': unittest.main()
