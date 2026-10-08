#include "qemu/osdep.h"
#include "ui/console.h"
#include "miband_display_bridge.h"

struct MiBandDisplayBridge {
    QemuConsole *console;
    unsigned width, height;
    bool valid;
};

static void display_invalidate(void *opaque) { }

static void display_update(void *opaque)
{
    MiBandDisplayBridge *bridge = opaque;
    if (bridge->valid) {
        dpy_gfx_update(bridge->console, 0, 0, bridge->width, bridge->height);
    }
}

static const GraphicHwOps display_ops = {
    .invalidate = display_invalidate,
    .gfx_update = display_update,
};

MiBandDisplayBridge *miband_display_bridge_new(unsigned width, unsigned height)
{
    MiBandDisplayBridge *bridge = g_new0(MiBandDisplayBridge, 1);
    bridge->width = width;
    bridge->height = height;
    bridge->console = graphic_console_init(NULL, 0, &display_ops, bridge);
    qemu_console_resize(bridge->console, width, height);
    /*
     * The VNC listener is created independently of the machine display
     * bridge. Select this console after it exists so listeners registered
     * before machine realization are rebound to the actual scanout surface.
     * Without this, dpy_gfx_update() is discarded while the console is not
     * active and browser clients remain connected to a black framebuffer.
     */
    console_select(qemu_console_get_index(bridge->console));
    return bridge;
}

void miband_display_bridge_present(void *opaque, const uint8_t *pixels,
                                    unsigned width, unsigned height)
{
    MiBandDisplayBridge *bridge = opaque;
    DisplaySurface *surface = qemu_console_surface(bridge->console);
    if (!surface || width != bridge->width || height != bridge->height) {
        return;
    }
    if (!pixels) {
        bridge->valid = false;
        memset(surface_data(surface), 0, (size_t)surface_stride(surface) * height);
    } else {
        if (surface_bits_per_pixel(surface) != 32) {
            return;
        }
        for (unsigned y = 0; y < height; y++) {
            memcpy(surface_data(surface) + (size_t)y * surface_stride(surface),
                   pixels + (size_t)y * width * 4, width * 4);
        }
        bridge->valid = true;
    }
    dpy_gfx_update(bridge->console, 0, 0, width, height);
}
