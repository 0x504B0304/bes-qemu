#ifndef MIBAND_PANEL_DEVICE_H
#define MIBAND_PANEL_DEVICE_H
#include "miband_panel_endpoint.h"

typedef struct MiBandPanelDevice MiBandPanelDevice;
/* Observed DCS subset; the physical panel/driver IC is not identified. */
MiBandPanelDevice *miband_panel_new(uint32_t id, unsigned width, unsigned height);
void miband_panel_free(MiBandPanelDevice *panel);
MiBandPanelEndpoint miband_panel_endpoint(MiBandPanelDevice *panel);
/* Packet-oriented DCS transport; unsupported reads return -1. Identity bytes
 * are most-significant first, as with the existing DA/DB/DC endpoint. */
int miband_panel_dcs_read(MiBandPanelDevice *panel, uint8_t command,
                         uint8_t *data, size_t capacity);
bool miband_panel_dcs_write(MiBandPanelDevice *panel, uint8_t command,
                           const uint8_t *data, size_t length);
/* NULL pixels means blank; dimensions and BGRA storage are immutable.
 * Presentation and TE callbacks execute only in the device owner context. */
void miband_panel_connect_display(MiBandPanelDevice *panel, void *opaque,
    void (*present)(void *, const uint8_t *, unsigned, unsigned));
void miband_panel_connect_te(MiBandPanelDevice *panel, void *opaque,
                             void (*edge)(void *));
#endif
