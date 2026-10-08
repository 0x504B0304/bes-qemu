#!/usr/bin/env python3
"""Test die identity/isolation and little-endian/bounds semantics without QEMU."""
from pathlib import Path
import subprocess
import tempfile
from tools.source_layout import source_file, include_flags


def main():
    program = r'''
#include "miband_psram_device.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint8_t storage[] = {0x11,0x22,0x33,0x44,0x55};
    MiBandPsramDevice a,b;
    uint32_t word = 0xabcdef;
    miband_psram_device_init(&a, storage, sizeof(storage), 0x0d);
    miband_psram_device_init(&b, storage, sizeof(storage), 0x42);
    assert(miband_psram_device_read_mode(&a,1)==0x0d);
    assert(miband_psram_device_read_mode(&b,1)==0x42);
    miband_psram_device_write_mode(&a,1,0);
    assert(miband_psram_device_read_mode(&a,1)==0x0d);
    miband_psram_device_write_mode(&a,0x104,0x12345678);
    assert(miband_psram_device_read_mode(&a,4)==0x12345678);
    assert(miband_psram_device_read_mode(&b,4)==0);
    assert(miband_psram_device_read_word(&a,0,&word) && word==0x44332211);
    assert(miband_psram_device_read_word(&a,1,&word) && word==0x55443322);
    assert(!miband_psram_device_read_word(&a,2,&word));
    assert(!miband_psram_device_read_word(&a,0xffffffff,&word));
    miband_psram_device_init(&a,storage,0,7);
    assert(!miband_psram_device_read_word(&a,0,&word));
    miband_psram_device_init(&a,storage,3,7);
    assert(!miband_psram_device_read_word(&a,0,&word));
    puts("PSRAM_DIE_OK isolated-MR readonly-identity little-endian bounds");
}
'''
    with tempfile.TemporaryDirectory(prefix='bes-psram-die-') as temp:
        temp = Path(temp)
        (temp/'test.c').write_text(program)
        subprocess.run(['clang-23', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=address,undefined', *include_flags(), str(temp/'test.c'),
            str(source_file('miband_psram_device.c')), '-o', str(temp/'test')], check=True)
        subprocess.run([str(temp/'test')], check=True, timeout=10)


if __name__ == '__main__':
    main()
