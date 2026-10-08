/* Private append-only flash store. Original firmware/resource files stay read-only.
 * Each completed guest flash operation is fdatasync'd before returning success.
 * This is a flash-state journal, not CPU/RAM migration. */
#include "miband_store.h"
#include <glib.h>
#include <zlib.h>
#include "bes_host_file.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOR_BYTES 0x40000u
#define PAGE_BYTES 2112u
#define PAGE_COUNT 262144u
#define MASS_MAX_BYTES (64u * 1024u * 1024u + 26u)
struct MiBandStore {
    int fd, lock;
    unsigned nor_bytes, page_bytes, page_count;
    uint64_t operations, elapsed_us, max_us, histogram[32];
};

static void fail(const char *why)
{
    fprintf(stderr, "miband flash store: %s (errno=%d)\n", why, errno);
    exit(EXIT_FAILURE);
}
static uint32_t get32(const uint8_t *b)
{ return (uint32_t)b[0] | (uint32_t)b[1]<<8 | (uint32_t)b[2]<<16 | (uint32_t)b[3]<<24; }
static void put32(uint8_t *b, uint32_t v)
{ for (unsigned i=0;i<4;i++) b[i]=v>>(8*i); }
static void write_all(int fd, const void *data, size_t length)
{
    const uint8_t *p=data;
    while (length) {
        ssize_t n=write(fd,p,length);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) fail("write failed; guest operation not acknowledged");
        p+=n;length-=n;
    }
}
static size_t read_all(int fd, void *data, size_t length)
{
    uint8_t *p=data;size_t used=0;
    while (used<length) {
        ssize_t n=read(fd,p+used,length-used);
        if (n<0 && errno==EINTR) continue;
        if (n<0) fail("read failed");
        if (!n) break;
        used+=n;
    }
    return used;
}
static void sync_file(int fd)
{ if (bes_data_sync(fd)) fail("fdatasync failed; guest operation not acknowledged"); }
static void validate(MiBandStore *s,unsigned type,unsigned addr,unsigned size)
{
    if ((type==MB_STORE_NOR && size && addr<s->nor_bytes && size<=s->nor_bytes-addr) ||
        (type==MB_STORE_PAGE && addr<s->page_count && size==s->page_bytes) ||
        (type==MB_STORE_ERASE && addr<s->page_count/64 && !size) ||
        (type==MB_STORE_MASS && addr <= 255 && size >= 26 && size <= MASS_MAX_BYTES)) return;
    fail("invalid record type/address/length");
}
static void truncate_tail(int fd, off_t offset)
{
    if (ftruncate(fd,offset) || lseek(fd,offset,SEEK_SET)<0) fail("torn tail recovery failed");
    sync_file(fd);
    fprintf(stderr,"miband flash store: recovered incomplete final record at %lld\n",(long long)offset);
}
MiBandStore *miband_store_open(const char *path,const uint8_t base[32],
                             const uint8_t *seed,MiBandRestore restore,void *opaque)
{
    return miband_store_open_geometry(path, base, seed, NOR_BYTES, PAGE_BYTES,
                                     PAGE_COUNT, restore, opaque);
}
MiBandStore *miband_store_open_geometry(const char *path,const uint8_t base[32],
                             const uint8_t *seed,unsigned nor_bytes,
                             unsigned page_bytes,unsigned page_count,
                             MiBandRestore restore,void *opaque)
{
    if (!nor_bytes || nor_bytes > 0x1000000 || nor_bytes % 4096 ||
        (page_bytes != PAGE_BYTES && page_bytes != 2176u) || !page_count || page_count > 0x1000000 ||
        page_count % 64) fail("unsupported flash geometry");
    MiBandStore *s=g_new0(MiBandStore,1);
    s->nor_bytes=nor_bytes;s->page_bytes=page_bytes;s->page_count=page_count;
    char *lock_path=g_strconcat(path,".lock",NULL);
    s->lock=g_open(lock_path,O_RDWR|O_CREAT|O_BINARY|O_CLOEXEC|O_NOFOLLOW,0600);
    g_free(lock_path);
    if (s->lock<0 || bes_file_lock(s->lock)) fail("state is locked or inaccessible");
    s->fd=g_open(path,O_RDWR|O_BINARY|O_CLOEXEC|O_NOFOLLOW,0600);
    uint8_t header[64]={0},*nor=g_malloc(nor_bytes);
    if (s->fd<0 && errno==ENOENT) {
        s->fd=g_open(path,O_RDWR|O_CREAT|O_EXCL|O_BINARY|O_CLOEXEC|O_NOFOLLOW,0600);
        if (s->fd<0) fail("cannot create state");
        memcpy(header,"MBFLASH1",8);
        put32(header+8,nor_bytes);put32(header+12,page_bytes);put32(header+16,page_count);
        put32(header+20,crc32(0,seed,nor_bytes));
        memcpy(header+32,base,32);
        write_all(s->fd,header,sizeof(header));write_all(s->fd,seed,nor_bytes);sync_file(s->fd);
        char *parent=g_path_get_dirname(path);
        if (bes_dir_sync(parent)) fail("directory sync failed");
        g_free(parent);
        memcpy(nor,seed,nor_bytes);
    } else {
        struct stat st;
        if (s->fd<0 || fstat(s->fd,&st) || !S_ISREG(st.st_mode)) fail("not a regular state file");
        if (read_all(s->fd,header,64)!=64 || memcmp(header,"MBFLASH1",8) ||
            get32(header+8)!=nor_bytes || get32(header+12)!=page_bytes ||
            get32(header+16)!=page_count || memcmp(header+32,base,32)) fail("wrong base image or state header");
        if (read_all(s->fd,nor,nor_bytes)!=nor_bytes || crc32(0,nor,nor_bytes)!=get32(header+20))
            fail("initial NOR checksum mismatch");
    }
    restore(opaque,MB_STORE_NOR,0,nor,nor_bytes);g_free(nor);
    uint8_t rec[16];
    for (;;) {
        off_t pos=lseek(s->fd,0,SEEK_CUR);
        if (pos<0) fail("seek failed");
        size_t got=read_all(s->fd,rec,sizeof(rec));
        if (!got) break;
        if (got<sizeof(rec)) { truncate_tail(s->fd,pos);break; }
        unsigned type=get32(rec),addr=get32(rec+4),size=get32(rec+8);
        validate(s,type,addr,size);
        uint8_t *data=g_malloc(MAX(size,1u));
        if (read_all(s->fd,data,size)!=size) { g_free(data);truncate_tail(s->fd,pos);break; }
        uint32_t crc=crc32(0,rec,12);
        crc=crc32(crc,data,size);
        if (crc!=get32(rec+12)) fail("record checksum mismatch; refusing silent data loss");
        restore(opaque,type,addr,data,size);g_free(data);
    }
    return s;
}
void miband_store_append(MiBandStore *s,unsigned type,unsigned addr,const uint8_t *data,unsigned size)
{
    if (!s) return;
    int64_t started = g_get_monotonic_time();
    validate(s,type,addr,size);
    uint8_t rec[16];put32(rec,type);put32(rec+4,addr);put32(rec+8,size);
    uint32_t crc=crc32(0,rec,12);
    /* zlib treats NULL as a CRC initialization request, including length 0. */
    if (size) crc=crc32(crc,data,size);
    put32(rec+12,crc);
    write_all(s->fd,rec,sizeof(rec));write_all(s->fd,data,size);sync_file(s->fd);
    uint64_t elapsed = g_get_monotonic_time() - started;
    unsigned bucket = 0;
    while (bucket < 31 && elapsed > (UINT64_C(1) << bucket)) bucket++;
    s->operations++;
    s->elapsed_us += elapsed;
    s->max_us = MAX(s->max_us, elapsed);
    s->histogram[bucket]++;
}
static uint64_t store_percentile(MiBandStore *s, unsigned percent)
{
    uint64_t count = 0, target = (s->operations * percent + 99) / 100;
    if (!target) return 0;
    for (unsigned i = 0; i < 32; i++) {
        count += s->histogram[i];
        if (count >= target) return UINT64_C(1) << i;
    }
    return s->max_us;
}
void miband_store_close(MiBandStore *s)
{
    if (s) {
        if (g_getenv("MIBAND_PERF_STATS"))
            fprintf(stderr, "miband store durable-sync ops=%" G_GUINT64_FORMAT
                    " total_us=%" G_GUINT64_FORMAT " max_us=%" G_GUINT64_FORMAT
                    " p50_upper_us=%" G_GUINT64_FORMAT
                    " p95_upper_us=%" G_GUINT64_FORMAT
                    " p99_upper_us=%" G_GUINT64_FORMAT "\n",
                    s->operations, s->elapsed_us, s->max_us,
                    store_percentile(s,50), store_percentile(s,95), store_percentile(s,99));
        close(s->fd); close(s->lock); g_free(s);
    }
}
