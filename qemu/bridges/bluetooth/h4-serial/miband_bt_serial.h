#ifndef MIBAND_BT_SERIAL_H
#define MIBAND_BT_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIBAND_SERIAL_PACKET_MAX 2048
#define MIBAND_SERIAL_RX_BYTES (512u * 1024u)
#define MIBAND_SERIAL_RX_PACKETS 1024u
#define MIBAND_SERIAL_TX_PACKETS 32u

typedef struct MiBandBtSerial MiBandBtSerial;

typedef struct MiBandSerialPacket {
    uint64_t epoch;
    uint64_t sequence;
    uint64_t received_ns;
    unsigned length;
    uint8_t data[MIBAND_SERIAL_PACKET_MAX];
} MiBandSerialPacket;

typedef struct MiBandSerialStats {
    uint64_t epoch;
    uint64_t rx_bytes, rx_packets, tx_bytes, tx_packets;
    uint64_t read_calls, read_eagain, read_eintr, write_eagain;
    uint64_t max_read_gap_ns, last_read_ns;
    uint64_t trace_dropped, trace_errors;
    uint64_t queue_oldest_ns;
    unsigned rx_count, rx_bytes_queued, rx_high_count, rx_high_bytes;
    unsigned tx_count, tx_high_count;
    bool failed;
    char error[160];
} MiBandSerialStats;

/* Called by the IO thread: only schedule a main-loop BH, never access a device.
 * The nonblocking fd is transferred only when this function succeeds. */
MiBandBtSerial *miband_serial_start(int fd, unsigned tx_bytes_per_second,
                                  const char *trace_path,
                                  void (*notify)(void *), void *opaque,
                                  char *error, size_t error_size);
void *miband_serial_opaque(MiBandBtSerial *serial);
bool miband_serial_send(MiBandBtSerial *serial, const uint8_t *data, unsigned len);
/* Single consumer. A peek does not free capacity; consume only after admission.
 * Neither function calls notify. A failed transport cannot deliver queued data. */
bool miband_serial_peek(MiBandBtSerial *serial, MiBandSerialPacket *packet);
void miband_serial_consume(MiBandBtSerial *serial);
void miband_serial_stats(MiBandBtSerial *serial, MiBandSerialStats *stats);
/* Caller first disables new users, then stops/joins before freeing BH/opaque. */
void miband_serial_stop(MiBandBtSerial *serial);

#endif
