"""Real eMMC commands with optional host read-ahead, including invalidation."""
import os
from pathlib import Path
import shutil
import subprocess
import struct
import tempfile
import unittest

from tests import test_best1700_emmc_qtest as base


class HostIoPolicy(unittest.TestCase):
    def test_actual_cache_code_io_counts_failure_fallback_and_overlap(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler unavailable')
        patch = (Path(__file__).resolve().parents[1] /
                 'qemu/machine/patches/sd-read-ahead/sd-read-ahead.patch').read_text()
        # Compile the actual patched functions, with a counted/faultable backend.
        source = '\n'.join(line[1:] for line in patch.splitlines()
                           if line[:1] in (' ', '+') and not line.startswith('+++'))
        def function(signature):
            start = source.index(signature)
            opening = source.index('{', start)
            depth = 1
            position = opening + 1
            while depth:
                depth += (source[position] == '{') - (source[position] == '}')
                position += 1
            return source[start:position] + '\n'
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define trace_sdcard_read_block(a,b) ((void)0)
#define trace_sdcard_write_block(a,b) ((void)0)
typedef struct { uint8_t data[131072]; int64_t length; int reads, writes;
                 bool fail_speculative, fail_all, fail_write; } BlockBackend;
typedef struct { BlockBackend *blk; uint32_t read_ahead_bytes;
                 uint8_t data[512], read_ahead_data[65536];
                 uint64_t read_ahead_base, partition_offset;
                 uint32_t read_ahead_valid; } SDState;
static int64_t blk_getlength(BlockBackend *b) { return b->length; }
static uint64_t sd_bootpart_offset(SDState *s) { return s->partition_offset; }
static int blk_pread(BlockBackend *b, uint64_t a, uint32_t n, void *dst, int flags)
{
    b->reads++;
    if (b->fail_all || (b->fail_speculative && n > 512) ||
        b->length < 0 || a > b->length || n > b->length - a) return -1;
    memcpy(dst, b->data + a, n); return 0;
}
static int blk_pwrite(BlockBackend *b, uint64_t a, uint32_t n, void *src, int flags)
{
    b->writes++;
    if (b->fail_write) { b->data[a] = 0x71; return -1; }
    memcpy(b->data + a, src, n); return 0;
}
'''
        code += ''.join(function(name) for name in
            ('static bool sd_read_ahead(', 'static void sd_blk_read(', 'static void sd_blk_write('))
        code += r'''
int main(void)
{
    static BlockBackend b = {.length=sizeof(b.data)};
    static SDState s = {.blk=&b, .read_ahead_bytes=65536};
    for (unsigned i=0; i<sizeof(b.data); i++) b.data[i] = (i*37+i/512)&255;
    for (unsigned i=0; i<128; i++) {
        sd_blk_read(&s,i*512,512);
        assert(!memcmp(s.data,b.data+i*512,512));
    }
    assert(b.reads == 1); /* 128 card blocks, one backend read. */
    sd_blk_write(&s,65536,512); /* Disjoint write retains the cached window. */
    sd_blk_read(&s,0,512); assert(b.reads == 1);
    memset(s.data,0x53,512); sd_blk_write(&s,512,512);
    sd_blk_read(&s,512,512); assert(b.reads == 2 && s.data[0]==0x53);
    b.fail_write = true; sd_blk_write(&s,512,512); b.fail_write=false;
    sd_blk_read(&s,512,512); assert(b.reads==3 && s.data[0]==0x71);
    s.partition_offset=65536; sd_blk_read(&s,512,512);
    assert(b.reads==4 && !memcmp(s.data,b.data+66048,512));
    s.read_ahead_valid=0; b.fail_speculative=true; b.reads=0;
    sd_blk_read(&s,512,512);
    assert(b.reads==2 && s.read_ahead_valid==0);
    assert(!memcmp(s.data,b.data+66048,512)); /* Exact-request fallback. */
    b.fail_speculative=false; b.length=65536+512;
    sd_blk_read(&s,0,512); assert(s.read_ahead_valid==512);
    assert(!memcmp(s.data,b.data+65536,512)); /* Clipped end of backend. */
    s.read_ahead_valid=0; b.length=-1; b.reads=0;
    assert(!sd_read_ahead(&s,0,512) && b.reads==0);
    b.length=sizeof(b.data); s.read_ahead_bytes=0; b.reads=0;
    sd_blk_read(&s,0,512); assert(b.reads==1 && s.read_ahead_valid==0);
    b.fail_all=true; s.read_ahead_bytes=65536; b.reads=0;
    memset(s.data,0x39,512); sd_blk_read(&s,0,512);
    assert(b.reads==2 && s.read_ahead_valid==0 && s.data[0]==0x39);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='sd-read-ahead-native-') as temp:
            path = Path(temp)
            (path/'probe.c').write_text(code)
            result = subprocess.run([compiler, '-std=c11', '-Wall', '-Werror',
                '-o', str(path/'probe'), str(path/'probe.c')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(path/'probe')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


@unittest.skipUnless(os.environ.get('MIBAND_BEST1700_QEMU'),
                     'MIBAND_BEST1700_QEMU is not set')
class CachedEmmc(base.Best1700EmmcQtest):
    # Run the existing PIO, SDMA, ADMA, IRQ and reset suite with the cache on.
    boot_bytes = 0
    aio = 'threads'

    def start_machine(self):
        self.machine = base.QtestMachine(
            os.environ['MIBAND_BEST1700_QEMU'],
            machine_options=',emmc-drive=emmc-test',
            extra_args=('-drive', f'if=none,id=emmc-test,file={self.disk},format=raw,aio={self.aio}' +
                        (',cache=none' if self.aio == 'native' else ''),
                        '-global', 'emmc.read-ahead-bytes=65536',
                        '-global', f'emmc.boot-partition-size={self.boot_bytes}'))
        self.machine.command('irq_intercept_in /machine/core[0]')

    @unittest.skipUnless(os.environ.get('MIBAND_TEST_NATIVE_AIO'),
                         'native host AIO regression is opt-in')
    def test_native_host_aio_read_write_and_restart(self):
        for engine in ('io_uring', 'native'):
            with self.subTest(engine=engine):
                self.close_machine()
                self.aio = engine
                self.start_machine()
                self.test_sdma_axi_alias_write_survives_host_system_and_process_reset()

    def test_cached_block_write_erase_and_card_reset(self):
        self.initialize_card()
        q = self.machine
        self.transfer(base.SEED_BLOCK)
        self.assertEqual(q.read(base.CPU_BUFFER, base.BLOCK), base.SEED[:base.BLOCK])
        replacement = bytes((i * 13 + 39) & 255 for i in range(base.BLOCK))
        q.write(base.CPU_BUFFER, replacement)
        self.transfer(base.SEED_BLOCK, write=True)
        self.transfer(base.SEED_BLOCK)
        self.assertEqual(q.read(base.CPU_BUFFER, base.BLOCK), replacement)
        # Erase must invalidate a block which has just been cached.
        argument = base.SEED_BLOCK if self.block_addressed else base.SEED_BLOCK * base.BLOCK
        self.command(35, argument)
        self.command(36, argument)
        self.command(38, response=base.R1B)
        self.transfer(base.SEED_BLOCK)
        self.assertEqual(q.read(base.CPU_BUFFER, base.BLOCK), bytes([255]) * base.BLOCK)
        self.initialize_card()  # CMD0/card reset, not merely host reset.
        self.transfer(base.SEED_BLOCK)
        self.assertEqual(q.read(base.CPU_BUFFER, base.BLOCK), bytes([255]) * base.BLOCK)

    def test_window_boundary_and_end_of_disk(self):
        self.initialize_card()
        q = self.machine
        for block in (127, 128, base.DISK_BYTES // base.BLOCK - 1):
            replacement = bytes((i * 43 + block) & 255 for i in range(base.BLOCK))
            q.write(base.CPU_BUFFER, replacement)
            self.transfer(block, write=True)
            self.transfer(block)
            self.assertEqual(q.read(base.CPU_BUFFER, base.BLOCK), replacement)
        self.transfer(127, blocks=2, adma=True)
        self.assertEqual(q.read(base.CPU_BUFFER, 2 * base.BLOCK),
            b''.join(bytes((i * 43 + block) & 255 for i in range(base.BLOCK))
                     for block in (127, 128)))

    def test_boot_partition_cache_keys_use_backend_offsets(self):
        self.close_machine()
        self.boot_bytes = 128 * 1024
        patterns = [bytes([v]) * base.BLOCK for v in (0x13, 0x27, 0x39)]
        with self.disk.open('r+b') as disk:
            for index, payload in enumerate(patterns):
                disk.seek(index * self.boot_bytes)
                disk.write(payload)
        self.start_machine()
        self.configure_host()
        self.command(0, response=0)
        ocr = self.command(1, 0x40FF8000, base.R3)
        self.block_addressed = bool(ocr & 0x40000000)
        self.command(2, response=base.R2)
        self.command(3, 1 << 16)
        self.command(7, 1 << 16, base.R1B)
        self.command(16, base.BLOCK)
        # SWITCH byte 179 PARTITION_CONFIG: 0=user, 1=boot0, 2=boot1.
        for partition, expected in ((0, 2), (1, 0), (2, 1), (0, 2), (1, 0)):
            self.command(6, (3 << 24) | (179 << 16) | (partition << 8), base.R1B)
            self.transfer(0)
            self.assertEqual(self.machine.read(base.CPU_BUFFER, base.BLOCK), patterns[expected])

    def test_qemu_rejects_invalid_property(self):
        for size in (1, 513, 131072):
            machine = base.QtestMachine(os.environ['MIBAND_BEST1700_QEMU'],
                machine_options=',emmc-drive=invalid-cache',
                extra_args=('-drive', f'if=none,id=invalid-cache,file={self.disk},format=raw',
                            '-global', f'emmc.read-ahead-bytes={size}'))
            try:
                machine.process.wait(timeout=5)
                self.assertNotEqual(machine.process.returncode, 0)
                self.assertIn('read-ahead-bytes must be', machine.stderr_path.read_text())
            finally:
                machine.close()


if __name__ == '__main__':
    unittest.main()
