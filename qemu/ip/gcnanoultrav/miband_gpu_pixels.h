#ifndef MIBAND_GPU_PIXELS_H
#define MIBAND_GPU_PIXELS_H

#include <stdbool.h>
#include <stdint.h>

/* GC NanoUltraV target state a10, including the read-destination bit.
 * BGR565/BGRA5658 put B in the low five bits, G in six, R in five;
 * BGRA5658 appends alpha. Three-byte formats encode pitch in four-byte units.
 * These encodings are verified against reference's target converter and the
 * VGLite target converter. A8 stores only the composited alpha channel. */
static inline unsigned miband_gpu_pixel_bytes(uint32_t format)
{
    switch (format) {
    case 0x106: return 1;
    case 0x101: case 0x105: return 2;
    case 0x109: case 0x10a: return 3;
    case 0x103: return 4;
    default: return 0;
    }
}

static inline bool miband_gpu_target_layout(uint32_t format,
                                            uint32_t register_stride,
                                            uint32_t width, uint32_t *stride)
{
    unsigned bytes = miband_gpu_pixel_bytes(format);
    *stride = register_stride;
    if (!bytes) {
        return false;
    }
    if (bytes == 3) {
        if (register_stride % 4) {
            return false;
        }
        *stride = register_stride / 4 * 3;
    } else if (register_stride % bytes) {
        return false;
    }
    return (uint64_t)*stride >= (uint64_t)width * bytes;
}

static inline uint32_t miband_gpu_pixel_read(const uint8_t *pixel,
                                             uint32_t format)
{
    uint32_t b, g, r, a;
    if (format == 0x106) {
        return (uint32_t)pixel[0] << 24;
    }
    if (format == 0x105) {
        unsigned packed = pixel[0] | ((unsigned)pixel[1] << 8);
        b = packed & 31;
        g = (packed >> 5) & 31;
        r = (packed >> 10) & 31;
        b = (b << 3) | (b >> 2);
        g = (g << 3) | (g >> 2);
        r = (r << 3) | (r >> 2);
        a = (packed & 0x8000) ? 255 : 0;
    } else if (format == 0x101 || format == 0x10a) {
        unsigned packed = pixel[0] | ((unsigned)pixel[1] << 8);
        b = packed & 31;
        g = (packed >> 5) & 63;
        r = (packed >> 11) & 31;
        b = (b << 3) | (b >> 2);
        g = (g << 2) | (g >> 4);
        r = (r << 3) | (r >> 2);
        a = format == 0x10a ? pixel[2] : 255;
    } else {
        b = pixel[0]; g = pixel[1]; r = pixel[2];
        a = format == 0x103 ? pixel[3] : 255;
    }
    return b | (g << 8) | (r << 16) | (a << 24);
}

static inline void miband_gpu_pixel_write(uint8_t *pixel, uint32_t format,
                                          uint32_t color)
{
    if (format == 0x106) {
        pixel[0] = color >> 24;
        return;
    }
    if (format == 0x105) {
        unsigned packed = ((color & 255) >> 3) |
                          (((color >> 8) & 255) >> 3) << 5 |
                          (((color >> 16) & 255) >> 3) << 10 |
                          ((color >> 31) << 15);
        pixel[0] = packed;
        pixel[1] = packed >> 8;
    } else if (format == 0x101 || format == 0x10a) {
        unsigned packed = ((color & 255) >> 3) |
                          (((color >> 8) & 255) >> 2) << 5 |
                          (((color >> 16) & 255) >> 3) << 11;
        pixel[0] = packed;
        pixel[1] = packed >> 8;
        if (format == 0x10a) {
            pixel[2] = color >> 24;
        }
    } else {
        pixel[0] = color; pixel[1] = color >> 8; pixel[2] = color >> 16;
        if (format == 0x103) {
            pixel[3] = color >> 24;
        }
    }
}

#endif
