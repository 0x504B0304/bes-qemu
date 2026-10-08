/* Observed command-panel compatibility subset, independent of QEMU/SoC. */
#include <glib.h>
#include <string.h>
#include "miband_panel_device.h"

struct MiBandPanelDevice {
    uint32_t id;
    unsigned width, height;
    size_t bytes;
    uint8_t *gram;
    MiBandPanelStatus state;
    bool display_on, te_enabled, partial_mode, idle_mode;
    uint8_t brightness, te_mode;
    void *display_opaque, *te_opaque;
    void (*present)(void *, const uint8_t *, unsigned, unsigned);
    void (*te_edge)(void *);
};

static void panel_notify(MiBandPanelDevice *panel)
{
    if (panel->present && (!panel->state.visible || panel->state.gram_valid)) {
        panel->present(panel->display_opaque,
            panel->state.visible ? panel->gram : NULL, panel->width, panel->height);
    }
}

static MiBandPanelStatus panel_status(void *opaque)
{
    return ((MiBandPanelDevice *)opaque)->state;
}

static MiBandPanelReply panel_command(void *opaque, uint8_t command, bool read)
{
    MiBandPanelDevice *panel = opaque;
    MiBandPanelReply reply = {0};
    bool was_visible = panel->state.visible;

    if (read) {
        switch (command) {
        case 0xda: reply.value = (panel->id >> 16) & 0xff; break;
        case 0xdb: reply.value = (panel->id >> 8) & 0xff; break;
        case 0xdc: reply.value = panel->id & 0xff; break;
        default: break;
        }
        return reply;
    }
    switch (command) {
    case 0x01:
        panel->display_on = false;
        panel->state.sleeping = true;
        panel->te_enabled = false;
        panel->te_mode = panel->brightness = 0;
        panel->partial_mode = panel->idle_mode = false;
        panel->state.memory_write = false;
        panel->state.gram_valid = false;
        memset(panel->gram, 0, panel->bytes);
        reply.reset_transfer = true;
        break;
    case 0x10: panel->state.sleeping = true; break;
    case 0x11: panel->state.sleeping = false; break;
    case 0x12: panel->partial_mode = true; break;
    case 0x13: panel->partial_mode = false; break;
    case 0x38: panel->idle_mode = false; break;
    case 0x39: panel->idle_mode = true; break;
    case 0x28: panel->display_on = false; break;
    case 0x29: panel->display_on = true; break;
    case 0x34: panel->te_enabled = false; break;
    case 0x35: panel->te_enabled = true; break;
    case 0x2c: panel->state.memory_write = true; break;
    case 0x2e: panel->state.memory_write = false; break;
    default: break;
    }
    panel->state.visible = panel->display_on && !panel->state.sleeping;
    /* Sleep/display-off retains GRAM, matching the prior compatibility model.
     * Real sleep retention, windows and panel timings need hardware evidence. */
    if (reply.reset_transfer || was_visible != panel->state.visible) {
        panel_notify(panel);
    }
    return reply;
}

static bool panel_transfer(void *opaque, const uint8_t *pixels, size_t size)
{
    MiBandPanelDevice *panel = opaque;
    if (!pixels || size != panel->bytes) {
        return false;
    }
    memcpy(panel->gram, pixels, size);
    panel->state.gram_valid = true;
    panel->state.frame_id++;
    panel_notify(panel);
    return true;
}

static const uint8_t *panel_snapshot(void *opaque)
{
    return ((MiBandPanelDevice *)opaque)->gram;
}

static void panel_tick(void *opaque)
{
    MiBandPanelDevice *panel = opaque;
    if (panel->te_edge && panel->te_enabled && panel->state.visible) {
        panel->te_edge(panel->te_opaque);
    }
}

MiBandPanelDevice *miband_panel_new(uint32_t id, unsigned width, unsigned height)
{
    if (id > 0xffffff || !width || !height || width > 1024 || height > 1024) {
        return NULL;
    }
    MiBandPanelDevice *panel = g_new0(MiBandPanelDevice, 1);
    panel->id = id;
    panel->width = width;
    panel->height = height;
    panel->bytes = (size_t)width * height * 4;
    panel->gram = g_malloc0(panel->bytes);
    panel->display_on = true;
    panel->state.visible = true;
    return panel;
}

void miband_panel_free(MiBandPanelDevice *panel)
{
    if (panel) {
        g_free(panel->gram);
        g_free(panel);
    }
}

MiBandPanelEndpoint miband_panel_endpoint(MiBandPanelDevice *panel)
{
    return (MiBandPanelEndpoint) {panel, panel_status, panel_command,
                                 panel_transfer, panel_snapshot, panel_tick};
}

int miband_panel_dcs_read(MiBandPanelDevice *panel, uint8_t command,
                         uint8_t *data, size_t capacity)
{
    uint8_t reply[3];
    size_t length = 1;

    switch (command) {
    case 0x04: /* GET_DISPLAY_ID */
        if (!panel->id) {
            return -1; /* No board identity configured. */
        }
        reply[0] = panel->id >> 16;
        reply[1] = panel->id >> 8;
        reply[2] = panel->id;
        length = 3;
        break;
    case 0x0a: /* GET_POWER_MODE: mode, sleep-out and display-on. */
        reply[0] = (panel->partial_mode ? 0x20 : 0x08) |
                   (panel->idle_mode ? 0x40 : 0) |
                   (panel->state.sleeping ? 0 : 0x10) |
                   (panel->display_on ? 0x04 : 0);
        break;
    case 0x0e: /* GET_SIGNAL_MODE: TE enabled and selected TE mode. */
        reply[0] = (panel->te_enabled ? 0x80 : 0) | (panel->te_mode << 6);
        break;
    case 0x52: reply[0] = panel->brightness; break;
    case 0xda: case 0xdb: case 0xdc:
        if (!panel->id) {
            return -1;
        }
        reply[0] = panel_command(panel, command, true).value;
        break;
    default:
        return -1;
    }
    if (!data || capacity < length) {
        return -1;
    }
    memcpy(data, reply, length);
    return length;
}

bool miband_panel_dcs_write(MiBandPanelDevice *panel, uint8_t command,
                           const uint8_t *data, size_t length)
{
    if (length && !data) {
        return false;
    }
    if (command == 0x51 || command == 0x35) {
        if (length != 1 || (command == 0x35 && data[0] > 1)) {
            return false;
        }
        if (command == 0x51) {
            panel->brightness = data[0];
        } else {
            panel->te_mode = data[0];
        }
    }
    panel_command(panel, command, false);
    return true;
}

void miband_panel_connect_display(MiBandPanelDevice *panel, void *opaque,
    void (*present)(void *, const uint8_t *, unsigned, unsigned))
{
    panel->display_opaque = opaque;
    panel->present = present;
    panel_notify(panel);
}

void miband_panel_connect_te(MiBandPanelDevice *panel, void *opaque,
                             void (*edge)(void *))
{
    panel->te_opaque = opaque;
    panel->te_edge = edge;
}
