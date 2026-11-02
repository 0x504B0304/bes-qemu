/*
 * MiBand virtual network card, guest-facing MMIO ABI 1.
 *
 * The guest sees a raw-IP tun-like FIFO.  The QEMU side is deliberately
 * backend neutral: today the standard netdev is slirp user networking, but a
 * future host/USB-WiFi passthrough backend can keep this ABI and replace only
 * the transport that feeds the Ethernet frames.
 */
#ifndef MIBAND_NET_H
#define MIBAND_NET_H

#include "qemu/osdep.h"

#define TYPE_MIBAND_NET "miband-net"

#define MIBAND_NET_ABI_VERSION 1u
#define MIBAND_NET_MAGIC 0x4d424e54u /* "MBNT" */
#define MIBAND_NET_MMIO_SIZE 0x1000u
#define MIBAND_NET_MTU 1500u
#define MIBAND_NET_FIFO_SIZE 1600u
#define MIBAND_NET_RX_QUEUE 8u

/* Register map.  FIFO windows are byte addressable; the guest may use
 * 8/16/32-bit stores, with trailing padding ignored by TX_LEN/RX_LEN. */
#define MIBAND_NET_REG_MAGIC       0x000
#define MIBAND_NET_REG_VERSION     0x004
#define MIBAND_NET_REG_STATUS      0x008
#define MIBAND_NET_REG_CONTROL     0x00c
#define MIBAND_NET_REG_TX_LEN      0x010
#define MIBAND_NET_REG_TX_KICK     0x014
#define MIBAND_NET_REG_RX_LEN      0x018
#define MIBAND_NET_REG_RX_ACK      0x01c
#define MIBAND_NET_REG_MAC_LO      0x020
#define MIBAND_NET_REG_MAC_HI      0x024
#define MIBAND_NET_REG_MTU         0x028
#define MIBAND_NET_REG_RX_PACKETS  0x030
#define MIBAND_NET_REG_TX_PACKETS  0x034
#define MIBAND_NET_REG_RX_DROPS    0x038
#define MIBAND_NET_REG_ERRORS      0x03c
#define MIBAND_NET_REG_READY       0x040
#define MIBAND_NET_REG_GUEST_STATE 0x044
#define MIBAND_NET_REG_GUEST_ERROR 0x048
#define MIBAND_NET_REG_TX_FIFO     0x100
#define MIBAND_NET_REG_RX_FIFO     0x800

#define MIBAND_NET_STATUS_LINK      (1u << 0)
#define MIBAND_NET_STATUS_TX_READY  (1u << 1)
#define MIBAND_NET_STATUS_RX_READY  (1u << 2)
#define MIBAND_NET_STATUS_FAULT     (1u << 3)

#define MIBAND_NET_CONTROL_RESET    (1u << 0)
#define MIBAND_NET_CONTROL_LINK     (1u << 1)

/* Host-side MMIO ERRORS values.  The guest config->error field has its own
 * small enum in firmware-runtime/net/net_abi.h. */
#define MIBAND_NET_ERR_NONE         0u
#define MIBAND_NET_ERR_TX_OVERSIZE  1u
#define MIBAND_NET_ERR_RX_OVERSIZE  2u
#define MIBAND_NET_ERR_RX_OVERFLOW  3u
#define MIBAND_NET_ERR_BACKEND      4u
#define MIBAND_NET_ERR_PROTOCOL     5u

#define MIBAND_NET_GUEST_STATE_STARTING 1u
#define MIBAND_NET_GUEST_STATE_READY    2u
#define MIBAND_NET_GUEST_STATE_FAILED   3u

struct MachineState;
struct MemoryRegion;
void miband_net_machine_init(struct MachineState *ms, struct MemoryRegion *memory,
                             const char *netdev,
                             Error **errp);

#endif
