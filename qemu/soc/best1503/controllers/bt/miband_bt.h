/* BEST1503 BT mailbox/controller adapter. Host transports are opaque bridges.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIBAND_BT_H
#define MIBAND_BT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "exec/hwaddr.h"
#include "exec/memory.h"
#include "hw/irq.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "miband_xiaomi_pb.h"

#define MIBAND_BT_PACKET_MAX 2048
#define MIBAND_BT_RX_QUEUE_LENGTH 32
#define MIBAND_BT_TRACE_QUEUE_LENGTH 32

typedef struct MiBandBt {
    struct MiBandHciBridge *bridge;
    struct MiBandHciSynthetic *synthetic;
    struct MiBandHciGatt *gatt;
    struct MiBandXiaomiMassReceiver *mass_receiver;
    MiBandXiaomiPbRequest pending_install;
    bool (*mass_commit)(void *, uint8_t, const uint8_t *, size_t);
    void *mass_commit_opaque;
    const uint8_t *mass_types;
    unsigned mass_type_count;
    uint32_t mass_slice_length;
    struct MiBandBtSerial *serial_worker;
    CPUState *serial_worker_cpu;
    uint32_t regs[4][0x10000 / 4];
    AddressSpace *as;
    uint8_t *ram;
    uint32_t mailbox_status;
    bool mailbox_pending[2];
    bool tx_done_pending[2];
    QEMUTimer *tx_done_event;
    QEMUTimer *rx_event[2];
    int64_t reply_ready_ns[2];
    uint8_t reply[2][MIBAND_BT_PACKET_MAX];
    unsigned reply_len[2];
    uint8_t rx_queue[2][MIBAND_BT_RX_QUEUE_LENGTH][MIBAND_BT_PACKET_MAX];
    uint16_t rx_queue_len[2][MIBAND_BT_RX_QUEUE_LENGTH];
    uint32_t rx_queue_generation[2][MIBAND_BT_RX_QUEUE_LENGTH];
    uint32_t reply_generation[2];
    uint32_t delivered_generation[2];
    uint16_t delivered_handle[2];
    unsigned rx_queue_head[2];
    unsigned rx_queue_count[2];
    bool controller_powered;
    bool controller_ready;
    bool controller_sleeping;
    bool host_requested;
    bool host_active;
    bool serial_active;
    bool serial_dual_mode;
    bool synthetic_peer_passive;
    uint8_t synthetic_last_write[MIBAND_BT_PACKET_MAX];
    unsigned synthetic_last_write_len;
    uint8_t synthetic_last_ap_packet[MIBAND_BT_PACKET_MAX];
    unsigned synthetic_last_ap_packet_len;
    unsigned synthetic_last_ap_channel;
    uint8_t synthetic_last_bt_packet[MIBAND_BT_PACKET_MAX];
    unsigned synthetic_last_bt_packet_len;
    uint8_t synthetic_ap_trace[MIBAND_BT_TRACE_QUEUE_LENGTH][MIBAND_BT_PACKET_MAX];
    uint16_t synthetic_ap_trace_len[MIBAND_BT_TRACE_QUEUE_LENGTH];
    uint8_t synthetic_ap_trace_channel[MIBAND_BT_TRACE_QUEUE_LENGTH];
    unsigned synthetic_ap_trace_head;
    unsigned synthetic_ap_trace_count;
    bool smp_just_works;
    uint32_t fixed_passkey;
    uint32_t le_passkey;
    bool le_smp_active;
    bool le_passkey_rand_pending;
    unsigned passkey_auto_replies;
    bool host_pending[2];
    uint16_t host_pending_opcode[2];
    uint32_t serial_tx_rate;
    uint16_t internal_cmd[4];
    unsigned internal_cmd_count;
    int64_t diagnostic_deadline_us;
    unsigned diagnostic_page;
    bool rx_flow_requested;
    bool rx_flow_active;
    unsigned rx_flow_stage;
    unsigned reset_channel;
    uint8_t saved_reset_complete[7];
    QEMUTimer *rx_flow_timer;
    uint32_t connection_sequence;
    uint32_t connection_generation[4096];
    bool connection_live[4096];
    unsigned rx_flow_received;
    unsigned rx_flow_returned;
    uint8_t bd_addr[6];
    uint8_t bd_addr_type;
    uint8_t local_features[8];
    uint8_t local_ext_features[8];
    uint8_t sleep_params[8];
    uint8_t wakeup_params[16];
    unsigned command_count;
    unsigned response_count;
    unsigned host_forwarded;
    unsigned host_events;
    qemu_irq tx_done[2];
    qemu_irq rx_data[2];
} MiBandBt;

void miband_bt_start_serial_worker(MiBandBt *, CPUState *, const char *);
void miband_bt_shutdown(MiBandBt *);
void miband_bt_set_rx_flow_requested(MiBandBt *, bool);
void miband_bt_set_serial_tx_rate(MiBandBt *, uint32_t);
void miband_bt_init(MiBandBt *bt, AddressSpace *as, uint8_t *ram);
bool miband_bt_request_transport_stats(MiBandBt *bt);
bool miband_bt_request_transport_debug(MiBandBt *bt, unsigned page);
void miband_bt_set_backend(MiBandBt *bt, const char *backend);
void miband_bt_set_gatt_mtu(MiBandBt *bt, uint16_t mtu);
void miband_bt_set_gatt_characteristics(MiBandBt *bt, const char *tx_uuid,
                                        const char *rx_uuid);
void miband_bt_set_synthetic_peer_passive(MiBandBt *bt, bool enabled);
bool miband_bt_inject_hci(MiBandBt *bt, unsigned channel,
                          const uint8_t *packet, unsigned size);
unsigned miband_bt_copy_synthetic_last_write(const MiBandBt *bt,
                                             uint8_t *packet, unsigned capacity);
unsigned miband_bt_copy_synthetic_last_ap_packet(const MiBandBt *bt,
                                                 uint8_t *packet,
                                                 unsigned capacity);
unsigned miband_bt_copy_synthetic_last_bt_packet(const MiBandBt *bt,
                                                 uint8_t *packet,
                                                 unsigned capacity);
unsigned miband_bt_pop_synthetic_ap_packet(MiBandBt *bt, uint8_t *packet,
                                            unsigned capacity,
                                            unsigned *channel);
void miband_bt_set_smp_just_works(MiBandBt *bt, bool enabled);
void miband_bt_set_fixed_passkey(MiBandBt *bt, uint32_t passkey);
void miband_bt_set_le_passkey(MiBandBt *bt, uint32_t passkey);
void miband_bt_set_mass_commit(MiBandBt *bt,
                               bool (*commit)(void *, uint8_t, const uint8_t *, size_t),
                               void *opaque);
void miband_bt_set_mass_types(MiBandBt *bt, const uint8_t *types, unsigned count);
void miband_bt_set_mass_slice_length(MiBandBt *bt, uint32_t length);
void miband_bt_connect_tx_irq(MiBandBt *bt, unsigned channel, qemu_irq irq);
void miband_bt_connect_rx_irq(MiBandBt *bt, unsigned channel, qemu_irq irq);
bool miband_bt_read(MiBandBt *bt, hwaddr address, unsigned size,
                    uint64_t *result);
bool miband_bt_write(MiBandBt *bt, hwaddr address, unsigned size,
                     uint64_t value);
bool miband_bt_mailbox_read(MiBandBt *bt, hwaddr address, unsigned size,
                            uint64_t *result);
bool miband_bt_mailbox_write(MiBandBt *bt, hwaddr address, unsigned size,
                             uint64_t value);

#endif
