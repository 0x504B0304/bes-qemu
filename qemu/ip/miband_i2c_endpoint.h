/* Byte-level I2C device interface. Address decoding belongs to the bus;
 * register prefixes and commands belong to the attached device. */
#ifndef MIBAND_I2C_ENDPOINT_H
#define MIBAND_I2C_ENDPOINT_H
#include <stdbool.h>
#include <stdint.h>

typedef struct MiBandI2cEndpoint {
    uint8_t address;
    void *opaque;
    void (*start)(void *opaque, bool read);
    void (*write)(void *opaque, uint8_t value);
    uint8_t (*read)(void *opaque);
    void (*stop)(void *opaque);
    void (*reset)(void *opaque);
} MiBandI2cEndpoint;
#endif
