/* Host serial/raw HCI transport; packet admission belongs to the SoC adapter.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "miband_hci_bridge.h"
#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <termios.h>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#include <sys/ioctl.h>
#endif
#else
#include <windows.h>
#include <io.h>
#endif
#include <unistd.h>
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef BTPROTO_HCI
#define BTPROTO_HCI 1
#endif
#ifndef HCI_CHANNEL_RAW
#define HCI_CHANNEL_RAW 0
#endif
struct miband_sockaddr_hci {
    unsigned short hci_family;
    unsigned short hci_dev, hci_channel;
};
#define MIBAND_BT_PACKET_MAX 2048
#define MIBAND_BT_HOST_TX_QUEUE_LENGTH 32
struct MiBandHciBridge {
    MiBandHciBridgeCallbacks callbacks;
    int host_fd;
    bool host_active, serial_active, strict_rx;
    uint8_t serial_rx[8192];
    unsigned serial_rx_len;
    uint8_t host_tx[MIBAND_BT_HOST_TX_QUEUE_LENGTH][MIBAND_BT_PACKET_MAX];
    uint16_t host_tx_len[MIBAND_BT_HOST_TX_QUEUE_LENGTH];
    unsigned host_tx_head, host_tx_count, host_tx_offset;
    unsigned serial_tx_rate;
    int64_t serial_tx_next_ns;
    QEMUTimer *serial_tx_timer;
};
static bool miband_hci_bridge_flush(MiBandHciBridge *bridge);
static void bridge_read_ready(void *opaque)
{
    MiBandHciBridge *bridge = opaque;
    if (bridge->host_active) bridge->callbacks.read_ready(bridge->callbacks.opaque);
}
static void bridge_write_ready(void *opaque)
{
    MiBandHciBridge *bridge = opaque;
    if (bridge->host_active) (void)miband_hci_bridge_flush(bridge);
}
static void bridge_failed(MiBandHciBridge *bridge, const char *reason)
{
    miband_hci_bridge_close(bridge);
    bridge->callbacks.failed(bridge->callbacks.opaque, reason);
}
MiBandHciBridge *miband_hci_bridge_new(MiBandHciBridgeCallbacks callbacks)
{
    MiBandHciBridge *bridge = g_new0(MiBandHciBridge, 1);
    bridge->callbacks = callbacks;
    bridge->host_fd = -1;
    bridge->serial_tx_timer = timer_new_ns(QEMU_CLOCK_REALTIME, bridge_write_ready, bridge);
    return bridge;
}
void miband_hci_bridge_close(MiBandHciBridge *bridge)
{
    if (!bridge) return;
    if (bridge->host_fd >= 0) {
        qemu_set_fd_handler(bridge->host_fd, NULL, NULL, NULL);
        close(bridge->host_fd);
        bridge->host_fd = -1;
    }
    bridge->host_active = bridge->serial_active = false;
    bridge->host_tx_head = bridge->host_tx_count = bridge->host_tx_offset = 0;
    bridge->serial_rx_len = 0;
    bridge->serial_tx_next_ns = 0;
    timer_del(bridge->serial_tx_timer);
}
void miband_hci_bridge_free(MiBandHciBridge *bridge)
{
    if (!bridge) return;
    miband_hci_bridge_close(bridge);
    timer_free(bridge->serial_tx_timer);
    g_free(bridge);
}
void miband_hci_bridge_set_rate(MiBandHciBridge *bridge, unsigned rate)
{
    bridge->serial_tx_rate = rate;
}
void miband_hci_bridge_set_strict_rx(MiBandHciBridge *bridge, bool strict)
{
    bridge->strict_rx = strict;
}
int miband_hci_bridge_detach_fd(MiBandHciBridge *bridge)
{
    assert(bridge->host_fd >= 0 && bridge->serial_active &&
           !bridge->host_tx_count && !bridge->serial_rx_len);
    qemu_set_fd_handler(bridge->host_fd, NULL, NULL, NULL);
    timer_del(bridge->serial_tx_timer);
    return bridge->host_fd;
}
void miband_hci_bridge_release_fd(MiBandHciBridge *bridge)
{
    bridge->host_fd = -1;
}
static bool miband_hci_bridge_flush(MiBandHciBridge *bridge)
{
    while (bridge->host_tx_count) {
        unsigned head = bridge->host_tx_head;
        unsigned length = bridge->host_tx_len[head];
        unsigned remaining = length - bridge->host_tx_offset;
        ssize_t sent;

        if (bridge->serial_active && bridge->serial_tx_rate) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            if (now < bridge->serial_tx_next_ns) {
                /* Never block the vCPU/UI, and do not spin on a writable fd
                 * while intentionally pacing. No saved tokens/idle bursts. */
                qemu_set_fd_handler(bridge->host_fd, bridge_read_ready, NULL, bridge);
                timer_mod(bridge->serial_tx_timer, bridge->serial_tx_next_ns);
                return false;
            }
            remaining = MIN(remaining, 64u);
        }

        sent = bridge->serial_active ?
            write(bridge->host_fd, bridge->host_tx[head] + bridge->host_tx_offset,
                  remaining) :
            send(bridge->host_fd, bridge->host_tx[head] + bridge->host_tx_offset,
                 length - bridge->host_tx_offset, MSG_DONTWAIT);
        if (sent > 0) {
            if (bridge->serial_active && bridge->serial_tx_rate) {
                int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                bridge->serial_tx_next_ns = now +
                    ((int64_t)sent * 1000000000 + bridge->serial_tx_rate - 1) /
                    bridge->serial_tx_rate;
                qemu_log_mask(LOG_UNIMP,
                              "miband BT serial paced bytes=%zd rate=%u time-ns=%" PRId64 "\n",
                              sent, bridge->serial_tx_rate, now);
            }
            bridge->host_tx_offset += sent;
            if (bridge->host_tx_offset == length) {
                bridge->host_tx_len[head] = 0;
                bridge->host_tx_head =
                    (head + 1) % MIBAND_BT_HOST_TX_QUEUE_LENGTH;
                bridge->host_tx_count--;
                bridge->host_tx_offset = 0;
            }
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                         errno == EINTR)) {
            qemu_set_fd_handler(bridge->host_fd,
                                bridge_read_ready,
                                bridge_write_ready, bridge);
            return false;
        }
        qemu_log_mask(LOG_UNIMP,
                      "miband BT host write failed sent=%zd errno=%d\n",
                      sent, errno);
        bridge_failed(bridge, "miband BT physical transport write failed; refusing synthetic fallback");
        return false;
    }
    qemu_set_fd_handler(bridge->host_fd,
                        bridge_read_ready,
                        NULL, bridge);
    return true;
}

bool miband_hci_bridge_queue(MiBandHciBridge *bridge, const uint8_t *data,
                                 unsigned size)
{
    if (!bridge->host_active || bridge->host_fd < 0 || !size || size > MIBAND_BT_PACKET_MAX ||
        bridge->host_tx_count >= MIBAND_BT_HOST_TX_QUEUE_LENGTH) {
        return false;
    }
    unsigned tail = (bridge->host_tx_head + bridge->host_tx_count) %
                    MIBAND_BT_HOST_TX_QUEUE_LENGTH;
    memcpy(bridge->host_tx[tail], data, size);
    bridge->host_tx_len[tail] = size;
    bridge->host_tx_count++;
    (void)miband_hci_bridge_flush(bridge);
    return bridge->host_active;
}

bool miband_hci_bridge_open_host(MiBandHciBridge *bridge, unsigned dev)
{
#ifndef __linux__
    /* Raw HCI sockets are a Linux interface. Native serial remains available. */
    return false;
#else
    struct miband_sockaddr_hci address;
    int flags;

    miband_hci_bridge_close(bridge);
    bridge->host_fd = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (bridge->host_fd < 0) {
        qemu_log_mask(LOG_UNIMP,
                      "miband BT host backend unavailable: socket errno=%d\n",
                      errno);
        return false;
    }

    memset(&address, 0, sizeof(address));
    address.hci_family = AF_BLUETOOTH;
    address.hci_dev = (unsigned short)dev;
    address.hci_channel = HCI_CHANNEL_RAW;
    if (bind(bridge->host_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        qemu_log_mask(LOG_UNIMP,
                      "miband BT host backend unavailable: bind hci%u errno=%d\n",
                      dev, errno);
        miband_hci_bridge_close(bridge);
        return false;
    }
    flags = fcntl(bridge->host_fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(bridge->host_fd, F_SETFL, flags | O_NONBLOCK);
    }
    bridge->host_active = true;
    qemu_set_fd_handler(bridge->host_fd, bridge_read_ready, NULL, bridge);
    qemu_log_mask(LOG_UNIMP, "miband BT host backend active hci%u\n", dev);
    return true;
#endif
}

#ifndef _WIN32
static speed_t bridge_serial_speed(unsigned baud)
{
    switch (baud) {
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1500000
    case 1500000: return B1500000;
#endif
#ifdef __APPLE__
    default: return baud ? B115200 : 0;
#else
    default: return 0;
#endif
    }
}

#endif

bool miband_hci_bridge_open_serial(MiBandHciBridge *bridge, const char *path,
                                  unsigned baud)
{
#ifdef _WIN32
    miband_hci_bridge_close(bridge);
    g_autofree char *device = g_str_has_prefix(path, "COM") ? g_strconcat("\\\\.\\", path, NULL) : g_strdup(path);
    HANDLE handle = CreateFileA(device, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DCB dcb = { .DCBlength = sizeof(DCB) };
    COMMTIMEOUTS timeout = { .ReadIntervalTimeout = MAXDWORD, .WriteTotalTimeoutConstant = 20 };
    if (!GetCommState(handle, &dcb)) { CloseHandle(handle); return false; }
    dcb.BaudRate = baud; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE; dcb.fOutxCtsFlow = FALSE; dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = dcb.fInX = FALSE; dcb.fDtrControl = DTR_CONTROL_DISABLE; dcb.fRtsControl = RTS_CONTROL_DISABLE;
    if (!SetCommState(handle, &dcb) || !SetCommTimeouts(handle, &timeout)) { CloseHandle(handle); return false; }
    int fd = _open_osfhandle((intptr_t)handle, _O_RDWR | _O_BINARY);
    if (fd < 0) { CloseHandle(handle); return false; }
#else
    struct termios term;
    speed_t speed;
    int fd;

    miband_hci_bridge_close(bridge);
    speed = bridge_serial_speed(baud);
    if (!speed) {
        qemu_log_mask(LOG_UNIMP,
                      "miband BT serial unsupported baud=%u\n", baud);
        return false;
    }
    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        qemu_log_mask(LOG_UNIMP,
                      "miband BT serial open failed path=%s errno=%d\n",
                      path, errno);
        return false;
    }
    if (tcgetattr(fd, &term) < 0) {
        qemu_log_mask(LOG_UNIMP,
                      "miband BT serial tcgetattr failed path=%s errno=%d\n",
                      path, errno);
        close(fd);
        return false;
    }
    cfmakeraw(&term);
    cfsetispeed(&term, speed);
    cfsetospeed(&term, speed);
    term.c_cflag |= CLOCAL | CREAD;
    term.c_cflag &= ~(CSTOPB | CRTSCTS | HUPCL);
    term.c_cc[VMIN] = 1;
    term.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &term) < 0) {
        qemu_log_mask(LOG_UNIMP,
                      "miband BT serial tcsetattr failed path=%s errno=%d\n",
                      path, errno);
        close(fd);
        return false;
    }
#endif
#ifdef __APPLE__
    speed_t requested_baud = baud;
    if (ioctl(fd, IOSSIOSPEED, &requested_baud) < 0) { close(fd); return false; }
#endif
    bridge->host_fd = fd;
    bridge->host_active = true;
    bridge->serial_active = true;
    bridge->serial_rx_len = 0;
#ifndef _WIN32
    qemu_set_fd_handler(fd, bridge_read_ready, NULL, bridge);
#endif
    qemu_log_mask(LOG_UNIMP,
                  "miband BT serial backend active path=%s baud=%u\n",
                  path, baud);
    return true;
}

static unsigned bridge_packet_length(const uint8_t *data,
                                           unsigned size)
{
    unsigned payload;

    if (size < 1) {
        return 0;
    }
    switch (data[0]) {
    case 0x01: /* command: opcode + parameter length */
        return size < 4 ? 0 : 4 + data[3];
    case 0x04: /* event: event code + parameter length */
        return size < 3 ? 0 : 3 + data[2];
    case 0x02: /* ACL: handle + data length */
        return size < 5 ? 0 : 5 + (data[3] | (data[4] << 8));
    case 0x03: /* SCO: handle + data length */
        return size < 4 ? 0 : 4 + data[3];
    case 0x05: /* ISO: handle + data length */
        if (size < 5) {
            return 0;
        }
        payload = (data[3] | (data[4] << 8)) & 0x3fff;
        return 5 + payload;
    default:
        return UINT_MAX;
    }
}

unsigned miband_hci_bridge_next_packet(MiBandHciBridge *bridge, uint8_t *packet)
{
    if (!bridge->host_active || bridge->host_fd < 0) return 0;
    if (!bridge->serial_active) {
        ssize_t size = recv(bridge->host_fd, packet, MIBAND_BT_PACKET_MAX, MSG_DONTWAIT);
        if (size < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            qemu_log_mask(LOG_UNIMP, "miband BT host receive failed errno=%d\n", errno);
        }
        return size > 0 ? size : 0;
    }
    for (;;) {
        while (bridge->serial_rx_len) {
            unsigned length = bridge_packet_length(bridge->serial_rx, bridge->serial_rx_len);
            if (length == UINT_MAX || length > MIBAND_BT_PACKET_MAX) {
                qemu_log_mask(LOG_UNIMP,
                              "miband BT serial invalid H4 header type=%02x len=%u\n",
                              bridge->serial_rx[0], length);
                if (bridge->strict_rx) {
                    bridge_failed(bridge, "physical H4 framing lost with RX flow enabled; refusing to forward guessed packets");
                    return 0;
                }
                memmove(bridge->serial_rx, bridge->serial_rx + 1, --bridge->serial_rx_len);
                continue;
            }
            if (!length || length > bridge->serial_rx_len) break;
            memcpy(packet, bridge->serial_rx, length);
            bridge->serial_rx_len -= length;
            memmove(bridge->serial_rx, bridge->serial_rx + length, bridge->serial_rx_len);
            return length;
        }
        uint8_t input[512];
        ssize_t received = read(bridge->host_fd, input, sizeof(input));
        if (received <= 0) {
            if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
            qemu_log_mask(LOG_UNIMP, "miband BT serial read failed errno=%d\n", errno);
            bridge_failed(bridge, "miband BT serial transport disconnected; refusing synthetic fallback");
            return 0;
        }
        if ((unsigned)received > sizeof(bridge->serial_rx) - bridge->serial_rx_len) {
            bridge_failed(bridge, "miband BT serial RX overflow; cannot preserve H4 stream");
            return 0;
        }
        memcpy(bridge->serial_rx + bridge->serial_rx_len, input, received);
        bridge->serial_rx_len += received;
    }
}
