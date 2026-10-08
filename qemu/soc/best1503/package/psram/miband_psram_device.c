/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "miband_psram_device.h"
#include <string.h>

void miband_psram_device_init(MiBandPsramDevice *device, uint8_t *storage,
                             uint32_t size, uint32_t identity)
{
    device->storage = storage;
    device->storage_size = size;
    memset(device->mode_registers, 0, sizeof(device->mode_registers));
    device->mode_registers[1] = identity;
}

uint32_t miband_psram_device_read_mode(MiBandPsramDevice *device, unsigned reg)
{
    return device->mode_registers[reg & 255];
}

void miband_psram_device_write_mode(MiBandPsramDevice *device, unsigned reg,
                                   uint32_t value)
{
    reg &= 255;
    if (reg != 1) {
        device->mode_registers[reg] = value;
    }
}

bool miband_psram_device_read_word(MiBandPsramDevice *device, uint32_t offset,
                                  uint32_t *value)
{
    const uint8_t *data;
    if (device->storage_size < 4 || offset > device->storage_size - 4) {
        return false;
    }
    data = device->storage + offset;
    *value = data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
             ((uint32_t)data[3] << 24);
    return true;
}
