/* Command-panel boundary; no SoC, guest memory or host display dependency. */
#ifndef MIBAND_PANEL_ENDPOINT_H
#define MIBAND_PANEL_ENDPOINT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct MiBandPanelStatus {
    bool visible, sleeping, gram_valid, memory_write;
    uint32_t frame_id;
} MiBandPanelStatus;

typedef struct MiBandPanelReply {
    uint32_t value;
    bool reset_transfer;
} MiBandPanelReply;

typedef struct MiBandPanelEndpoint {
    void *opaque;
    MiBandPanelStatus (*status)(void *opaque);
    MiBandPanelReply (*command)(void *opaque, uint8_t command, bool read);
    /* Full-frame compatibility transport, tightly packed BGRA8888.
     * The endpoint copies the borrowed input before returning. */
    bool (*transfer)(void *opaque, const uint8_t *pixels, size_t size);
    /* Borrowed read-only GRAM for owner-context diagnostics only. */
    const uint8_t *(*snapshot)(void *opaque);
    void (*tick)(void *opaque);
} MiBandPanelEndpoint;
#endif
