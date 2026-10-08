#!/usr/bin/env python3
"""Exercise real NOR/NAND device code without a SoC under ASan/UBSan."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
from tools.source_layout import source_file, include_flags


def main():
    program = r'''
#include "miband_nor_device.h"
#include "miband_nand_device.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    uint8_t window[64], id[] = {0xc8, 0x60, 0x17}, value;
    MiBandNorDevice *nor = miband_nor_new(window, 0x1000, 64, 16, id);
    MiBandFlashEndpoint n = miband_nor_endpoint(nor);
    const uint8_t dual[] = {0xcc,0x88,0x66,0x00,0x11,0x77};
    for (unsigned i=0;i<3;i++) {
        assert(n.response(n.opaque,0x9f,i,3,&value) && value==id[i]);
    }
    for (unsigned i=0;i<6;i++) {
        assert(n.response(n.opaque,0x9f,i,6,&value) && value==dual[i]);
    }
    uint8_t bytes[] = {0xaa,0x55,0xff,0x33};
    n.commit_tx(n.opaque,0x32,0x1000,bytes,4);
    assert(!memcmp(window,bytes,4));
    memset(bytes,0x0f,4);
    n.commit_tx(n.opaque,0x32,0x1000,bytes,4);
    assert(window[0]==0x0a && window[1]==0x05 && window[2]==0x0f);
    n.commit_tx(n.opaque,0x32,0x0fff,bytes,4);
    n.commit_tx(n.opaque,0x32,0x103f,bytes,4);
    assert(window[0]==0x0a && window[63]==0xff);
    n.command(n.opaque,0x20,0x1007,NULL,0);
    for (unsigned i=0;i<64;i++) assert(window[i]==0xff);
    miband_nor_free(nor);

    /* A second geometry proves the protocol does not need BEST1503 structs. */
    MiBandNandGeometry geometry = {4,2,2,4,{0xc8,0xf4,0xc8}};
    MiBandNandDevice *nand = miband_nand_new(&geometry);
    MiBandFlashEndpoint f = miband_nand_endpoint(nand);
    assert(f.cache_size==6 && !miband_nand_store_compatible(nand));
    for (unsigned i=0;i<3;i++) {
        assert(f.response(f.opaque,0x9f,i,3,&value) && value==geometry.jedec_id[i]);
    }
    assert(f.response(f.opaque,0x9f,0,6,&value) && value==0);
    uint8_t image[] = {1,2,3,4,5,6,7,8};
    miband_nand_set_resource(nand,image,sizeof(image),1,false);
    f.command(f.opaque,0x13,0,NULL,0);
    for (unsigned i=0;i<6;i++) assert(f.read_cache(f.opaque)[i]==0xff);
    f.command(f.opaque,0x13,1,NULL,0);
    assert(!memcmp(f.read_cache(f.opaque),image,4));
    miband_nand_set_image(nand,image,sizeof(image),false);
    assert(f.command(f.opaque,0x13,1,NULL,0)==MB_FLASH_CACHE_REFRESH);
    assert(!memcmp(f.read_cache(f.opaque),image+4,4));
    assert(f.read_cache(f.opaque)[4]==0xff && f.read_cache(f.opaque)[5]==0xff);
    assert(f.command(f.opaque,0x32,0,(uint8_t[]){0xf0,0x0f},2)==MB_FLASH_DMA_WRITE);
    uint8_t *spare=f.program_buffer(f.opaque,4,2);
    assert(spare);spare[0]=0xaa;spare[1]=0x55;
    assert(!f.program_buffer(f.opaque,5,2));
    assert(!f.program_buffer(f.opaque,UINT32_MAX,1));
    f.command(f.opaque,0x10,2,NULL,0);
    assert(!f.program_buffer(f.opaque,0,1));
    f.command(f.opaque,0x13,2,NULL,0);
    assert(!memcmp(f.read_cache(f.opaque),(uint8_t[]){0xf0,0x0f,0xff,0xff,0xaa,0x55},6));
    f.command(f.opaque,0x32,0,(uint8_t[]){0x0f,0xf0,0x11,0x22},4);
    f.command(f.opaque,0x84,4,(uint8_t[]){0x0f,0xf0},2);
    f.command(f.opaque,0x10,2,NULL,0);
    f.command(f.opaque,0x13,2,NULL,0);
    assert(!memcmp(f.read_cache(f.opaque),(uint8_t[]){0,0,0x11,0x22,0x0a,0x50},6));
    f.command(f.opaque,0xd8,1,NULL,0);
    f.command(f.opaque,0x13,0,NULL,0);
    for (unsigned i=0;i<6;i++) assert(f.read_cache(f.opaque)[i]==0xff);
    f.command(f.opaque,0x13,1,NULL,0);
    for (unsigned i=0;i<6;i++) assert(f.read_cache(f.opaque)[i]==0xff);
    f.command(f.opaque,0x13,2,NULL,0);
    assert(f.read_cache(f.opaque)[2]==0x11); /* other block remains written */
    miband_nand_free(nand);

    nand=miband_nand_new(&geometry);f=miband_nand_endpoint(nand);
    uint8_t raw[] = {1,2,3,4,0xa1,0xa2,5,6,7,8,0xb1,0xb2};
    miband_nand_set_resource(nand,raw,sizeof(raw),1,true);
    f.command(f.opaque,0x13,0,NULL,0);
    for (unsigned i=0;i<6;i++) assert(f.read_cache(f.opaque)[i]==0xff);
    f.command(f.opaque,0x13,1,NULL,0);
    assert(!memcmp(f.read_cache(f.opaque),raw,6));
    f.command(f.opaque,0x13,2,NULL,0);
    assert(!memcmp(f.read_cache(f.opaque),raw+6,6));
    miband_nand_set_image(nand,raw,sizeof(raw),true);
    f.command(f.opaque,0x13,1,NULL,0);
    assert(!memcmp(f.read_cache(f.opaque),raw+6,6));
    f.command(f.opaque,0x13,geometry.page_count,NULL,0);
    for (unsigned i=0;i<6;i++) assert(f.read_cache(f.opaque)[i]==0xff);
    assert(f.command(f.opaque,0xeb,0,NULL,0)==MB_FLASH_DMA_READ);
    miband_nand_free(nand);
    puts("FLASH_DEVICES_OK isolated geometry raw-spare bitwise-program erase-overlay bounds");
    geometry=(MiBandNandGeometry){2048,128,64,131072,{0xc8,0x82,0xc8}};
    nand=miband_nand_new(&geometry);f=miband_nand_endpoint(nand);
    assert(f.cache_size==2176 && miband_nand_store_compatible(nand));
    for(unsigned i=0;i<3;i++) {
        assert(f.response(f.opaque,0x9f,i,3,&value) && value==geometry.jedec_id[i]);
    }
    f.command(f.opaque,0x32,0,NULL,0);
    uint8_t *full=f.program_buffer(f.opaque,0,2176);
    assert(full);memset(full,0x55,2176);
    assert(!f.program_buffer(f.opaque,2175,2));
    f.command(f.opaque,0x10,131071,NULL,0);
    f.command(f.opaque,0x13,131071,NULL,0);
    for(unsigned i=0;i<2176;i++) assert(f.read_cache(f.opaque)[i]==0x55);
    for(unsigned i=0;i<6;i++) {
        const uint8_t read_ops[]={0x03,0x0b,0x3b,0x6b,0xbb,0xeb};
        assert(f.command(f.opaque,read_ops[i],0,NULL,0)==MB_FLASH_DMA_READ);
        for(unsigned j=0;j<2176;j++) assert(f.read_cache(f.opaque)[j]==0x55);
    }
    f.command(f.opaque,0x13,131072,NULL,0);
    for(unsigned i=0;i<2176;i++) assert(f.read_cache(f.opaque)[i]==0xff);
    miband_nand_free(nand);
}
'''
    with tempfile.TemporaryDirectory(prefix='bes-flash-devices-') as directory:
        directory = Path(directory)
        (directory/'test.c').write_text(program)
        flags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'glib-2.0', 'zlib'], text=True))
        subprocess.run(['clang-23', '-Wall', '-Wextra', '-Werror',
            '-Wno-unused-parameter', '-fsanitize=address,undefined', *include_flags(),
            str(directory/'test.c'), str(source_file('miband_nor_device.c')),
            str(source_file('miband_nand_device.c')), str(source_file('miband_store.c')),
            *flags, '-o', str(directory/'test')], check=True)
        subprocess.run([str(directory/'test')], check=True, timeout=10,
                       env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})


if __name__ == '__main__':
    main()
