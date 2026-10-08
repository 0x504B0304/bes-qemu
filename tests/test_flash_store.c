#include "miband_store.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static uint8_t nor[0x40000], pages[64][2176], present[64];
static void restore(void *unused,unsigned type,unsigned addr,const uint8_t *data,unsigned size)
{
    if(type==MB_STORE_NOR) memcpy(nor+addr,data,size);
    else if(type==MB_STORE_PAGE) {assert(addr<64);memcpy(pages[addr],data,size);present[addr]=1;}
    else {assert(addr==0);memset(present,0,sizeof(present));}
}
int main(int argc,char **argv)
{
    assert(argc==3 || argc==4);uint8_t base[32]={1},page[2176];memset(nor,255,sizeof(nor));
    if(!strcmp(argv[1],"badbase")) base[0]=2;
    unsigned nor_size = argc==4 ? 0x4000 : sizeof(nor);
    unsigned page_count = argc==4 ? 131072 : 262144;
    unsigned page_size = argc==4 ? 2176 : 2112;
    MiBandStore *s = argc==4 ?
        miband_store_open_geometry(argv[2],base,nor,nor_size,page_size,page_count,restore,NULL) :
        miband_store_open(argv[2],base,nor,restore,NULL);
    if(!strcmp(argv[1],"badnor"))
        miband_store_append(s,MB_STORE_NOR,nor_size,(const uint8_t *)"x",1);
    if(!strcmp(argv[1],"badpage"))
        miband_store_append(s,MB_STORE_PAGE,page_count,page,page_size);
    if(!strcmp(argv[1],"write")) {
        miband_store_append(s,MB_STORE_NOR,17,(const uint8_t *)"abc",3);
        miband_store_append(s,MB_STORE_NOR,nor_size-1,(const uint8_t *)"z",1);
        memset(page,0x33,sizeof(page));miband_store_append(s,MB_STORE_PAGE,5,page,page_size);
        miband_store_append(s,MB_STORE_ERASE,0,NULL,0);
        memset(page,0xab,sizeof(page));miband_store_append(s,MB_STORE_PAGE,9,page,page_size);
        _exit(0); /* No close/atexit: every completed record must already persist. */
    }
    assert(!memcmp(nor+17,"abc",3));assert(!present[5] && present[9]);
    assert(nor[nor_size-1]=='z');
    for(unsigned i=0;i<page_size;i++) assert(pages[9][i]==0xab);
    if(!strcmp(argv[1],"hold")) {puts("LOCKED");fflush(stdout);sleep(30);}
    miband_store_close(s);return 0;
}
