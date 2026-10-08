/* PSRAM die behavior. Independent of controller MMIO, aliases and firmware. */
#ifndef MIBAND_PSRAM_DEVICE_H
#define MIBAND_PSRAM_DEVICE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct MiBandPsramDevice {
    uint8_t *storage;
    uint32_t storage_size;
    uint32_t mode_registers[256];
} MiBandPsramDevice;

void miband_psram_device_init(MiBandPsramDevice *device, uint8_t *storage,
                             uint32_t size, uint32_t identity);
uint32_t miband_psram_device_read_mode(MiBandPsramDevice *device, unsigned reg);
void miband_psram_device_write_mode(MiBandPsramDevice *device, unsigned reg,
                                   uint32_t value);
bool miband_psram_device_read_word(MiBandPsramDevice *device, uint32_t offset,
                                  uint32_t *value);
#endif
