/* Host H4 serial/raw HCI transport. No guest memory, MMIO or board dependency. */
#ifndef MIBAND_HCI_BRIDGE_H
#define MIBAND_HCI_BRIDGE_H
#include <stdbool.h>
#include <stdint.h>

typedef struct MiBandHciBridge MiBandHciBridge;
typedef struct MiBandHciBridgeCallbacks {
    void *opaque;
    void (*read_ready)(void *opaque);
    void (*failed)(void *opaque, const char *reason);
} MiBandHciBridgeCallbacks;

MiBandHciBridge *miband_hci_bridge_new(MiBandHciBridgeCallbacks callbacks);
void miband_hci_bridge_close(MiBandHciBridge *bridge);
void miband_hci_bridge_free(MiBandHciBridge *bridge);
bool miband_hci_bridge_open_serial(MiBandHciBridge *bridge, const char *path, unsigned baud);
bool miband_hci_bridge_open_host(MiBandHciBridge *bridge, unsigned dev);
void miband_hci_bridge_set_rate(MiBandHciBridge *bridge, unsigned bytes_per_second);
void miband_hci_bridge_set_strict_rx(MiBandHciBridge *bridge, bool strict);
bool miband_hci_bridge_queue(MiBandHciBridge *bridge, const uint8_t *data, unsigned size);
/* Complete packets include the H4 type byte. Zero means no complete packet.
 * Serial partial reads stay buffered; raw HCI sockets already delimit packets.
 * Caller supplies a buffer of at least 2048 bytes. No guest state is touched. */
unsigned miband_hci_bridge_next_packet(MiBandHciBridge *bridge, uint8_t *packet);
/* Borrow after removing legacy handlers; ownership transfers only on release.
 * Startup worker handoff requires empty TX and RX buffers. */
int miband_hci_bridge_detach_fd(MiBandHciBridge *bridge);
void miband_hci_bridge_release_fd(MiBandHciBridge *bridge);
#endif
