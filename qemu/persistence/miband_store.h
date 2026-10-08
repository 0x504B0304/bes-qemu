#ifndef MIBAND_STORE_H
#define MIBAND_STORE_H
#include <stdint.h>
#include <stddef.h>
typedef struct MiBandStore MiBandStore;
enum { MB_STORE_NOR = 1, MB_STORE_PAGE = 2, MB_STORE_ERASE = 3,
       MB_STORE_MASS = 4 };
typedef void (*MiBandRestore)(void *, unsigned, unsigned, const uint8_t *, unsigned);
MiBandStore *miband_store_open(const char *, const uint8_t[32], const uint8_t *,
                             MiBandRestore, void *);
/* MBFLASH1 records carry geometry in the header; erase addresses are 64-page
 * block indices. The legacy entry point retains its original geometry. */
MiBandStore *miband_store_open_geometry(const char *, const uint8_t[32],
                                      const uint8_t *, unsigned, unsigned,
                                      unsigned, MiBandRestore, void *);
void miband_store_append(MiBandStore *, unsigned, unsigned, const uint8_t *, unsigned);
void miband_store_close(MiBandStore *);
#endif
