#ifndef MIBAND_DISPLAY_BRIDGE_H
#define MIBAND_DISPLAY_BRIDGE_H
#include <stdint.h>
typedef struct MiBandDisplayBridge MiBandDisplayBridge;
MiBandDisplayBridge *miband_display_bridge_new(unsigned width, unsigned height);
/* Owner-context callback; NULL pixels blanks the host surface. */
void miband_display_bridge_present(void *opaque, const uint8_t *pixels,
                                    unsigned width, unsigned height);
#endif
