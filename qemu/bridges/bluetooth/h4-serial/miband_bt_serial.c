/* Linux H4 transport. No guest state, QEMU lock, or virtual clock is used here.
 * The standalone build exercises this exact transport against PTYs. */
#ifndef MIBAND_SERIAL_STANDALONE
#include "qemu/osdep.h"
#else
#define _GNU_SOURCE
#endif
#include "miband_bt_serial.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/eventfd.h>
#endif
#ifdef _WIN32
#ifdef MIBAND_SERIAL_STANDALONE
#include <winsock2.h>
#endif
#include <windows.h>
#include <io.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC O_NOINHERIT
#endif
#endif
#include <time.h>
#include <unistd.h>

#define TRACE_COUNT 128u
#define READ_SIZE 4096u
#define IO_BATCH 8u

typedef struct SerialTrace {
    uint64_t ns;
    unsigned length;
    unsigned direction;
    uint8_t data[READ_SIZE];
} SerialTrace;

struct MiBandBtSerial {
    pthread_t io_thread, trace_thread;
    pthread_mutex_t lock;
    pthread_cond_t trace_cond;
    int fd, wake_fd, wake_write, trace_fd;
#ifdef _WIN32
    HANDLE wake_event;
#endif
    bool stop, trace_started, trace_stop, trace_failed;
    unsigned rate;
    void (*notify)(void *);
    void *opaque;
    MiBandSerialStats stats;
    MiBandSerialPacket rx[MIBAND_SERIAL_RX_PACKETS];
    unsigned rx_head;
    uint8_t partial[MIBAND_SERIAL_PACKET_MAX];
    unsigned partial_len;
    uint8_t tx[MIBAND_SERIAL_TX_PACKETS][MIBAND_SERIAL_PACKET_MAX];
    unsigned tx_len[MIBAND_SERIAL_TX_PACKETS];
    unsigned tx_head, tx_offset;
    uint64_t tx_next_ns;
    SerialTrace trace[TRACE_COUNT];
    unsigned trace_head, trace_count;
};

static pthread_mutex_t epoch_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t next_epoch;

static uint64_t serial_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void serial_wake(MiBandBtSerial *s)
{
#ifdef _WIN32
    SetEvent(s->wake_event);
#else
    uint64_t value = 1;
    ssize_t n;
    do {
        n = write(s->wake_write, &value, sizeof(value));
    } while (n < 0 && errno == EINTR);
    /* EAGAIN means an unread wakeup already exists. */
#endif
}

static void serial_fail(MiBandBtSerial *s, const char *reason, int error)
{
    bool first;
    pthread_mutex_lock(&s->lock);
    first = !s->stats.failed;
    if (first) {
        s->stats.failed = true;
        snprintf(s->stats.error, sizeof(s->stats.error), "%s (errno=%d)",
                 reason, error);
    }
    pthread_mutex_unlock(&s->lock);
    if (first) {
        s->notify(s->opaque);
        serial_wake(s);
    }
}

static void serial_trace(MiBandBtSerial *s, unsigned direction,
                         const uint8_t *data, unsigned length, uint64_t now)
{
    SerialTrace *entry;
    if (!s->trace_started) {
        return;
    }
    pthread_mutex_lock(&s->lock);
    if (s->trace_failed || s->trace_count == TRACE_COUNT) {
        s->stats.trace_dropped++;
    } else {
        entry = &s->trace[(s->trace_head + s->trace_count) % TRACE_COUNT];
        entry->ns = now;
        entry->direction = direction;
        entry->length = length;
        memcpy(entry->data, data, length);
        s->trace_count++;
        pthread_cond_signal(&s->trace_cond);
    }
    pthread_mutex_unlock(&s->lock);
}

static bool trace_write(int fd, const void *data, unsigned length)
{
    const uint8_t *p = data;
    while (length) {
        ssize_t n = write(fd, p, length);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        length -= n;
    }
    return true;
}

static void put_le64(uint8_t *p, uint64_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        p[i] = value >> (i * 8);
    }
}

static void *serial_trace_thread(void *opaque)
{
    MiBandBtSerial *s = opaque;
    SerialTrace entry;
    uint64_t sequence = 0;
    bool ok = true;
    #ifdef __APPLE__
    pthread_setname_np("miband-hci-log");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "miband-hci-log");
#endif
    /* MBHCI001, then records: LE64 monotonic ns, LE64 epoch, LE64 sequence,
     * LE32 length, byte direction (0=RX,1=TX), three reserved bytes, raw bytes. */
    ok = trace_write(s->trace_fd, "MBHCI001", 8);
    if (!ok) {
        pthread_mutex_lock(&s->lock);
        s->trace_failed = true;
        s->stats.trace_errors++;
        pthread_mutex_unlock(&s->lock);
    }
    for (;;) {
        uint8_t header[32] = {0};
        pthread_mutex_lock(&s->lock);
        while (!s->trace_count && !s->trace_stop) {
            pthread_cond_wait(&s->trace_cond, &s->lock);
        }
        if (!s->trace_count && s->trace_stop) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        entry = s->trace[s->trace_head];
        s->trace_head = (s->trace_head + 1) % TRACE_COUNT;
        s->trace_count--;
        pthread_mutex_unlock(&s->lock);
        put_le64(header, entry.ns);
        put_le64(header + 8, s->stats.epoch);
        put_le64(header + 16, ++sequence);
        for (unsigned i = 0; i < 4; i++) {
            header[24 + i] = entry.length >> (i * 8);
        }
        header[28] = entry.direction;
        if (ok) {
            ok = trace_write(s->trace_fd, header, sizeof(header)) &&
                 trace_write(s->trace_fd, entry.data, entry.length);
            if (!ok) {
                pthread_mutex_lock(&s->lock);
                s->trace_failed = true;
                s->stats.trace_errors++;
                pthread_mutex_unlock(&s->lock);
            }
        } else {
            pthread_mutex_lock(&s->lock);
            s->stats.trace_dropped++;
            pthread_mutex_unlock(&s->lock);
        }
    }
    close(s->trace_fd);
    return NULL;
}

/* Zero means a partial header; UINT_MAX is invalid. Check the length as soon
 * as the header exists, including when a complete oversized packet arrived. */
static unsigned h4_length(const uint8_t *p, unsigned n)
{
    switch (p[0]) {
    case 1: return n < 4 ? 0 : 4 + p[3];
    case 2: return n < 5 ? 0 : 5 + (p[3] | p[4] << 8);
    case 3: return n < 4 ? 0 : 4 + p[3];
    case 4: return n < 3 ? 0 : 3 + p[2];
    case 5: return n < 5 ? 0 : 5 + ((p[3] | p[4] << 8) & 0x3fff);
    default: return UINT_MAX;
    }
}

static bool serial_parse(MiBandBtSerial *s, const uint8_t *data,
                          unsigned length, uint64_t now)
{
    bool notify = false;
    for (unsigned i = 0; i < length; i++) {
        unsigned wanted;
        /* Copying one byte has no formatting/allocation or external effects;
         * a maximum 2 KiB partial packet is private to this worker. */
        s->partial[s->partial_len++] = data[i];
        wanted = h4_length(s->partial, s->partial_len);
        if (wanted == UINT_MAX || wanted > MIBAND_SERIAL_PACKET_MAX) {
            serial_fail(s, "H4 framing lost; refusing guessed packets", 0);
            return false;
        }
        if (wanted && wanted == s->partial_len) {
            MiBandSerialPacket *packet;
            pthread_mutex_lock(&s->lock);
            if (s->stats.rx_count == MIBAND_SERIAL_RX_PACKETS ||
                wanted > MIBAND_SERIAL_RX_BYTES - s->stats.rx_bytes_queued) {
                pthread_mutex_unlock(&s->lock);
                serial_fail(s, "physical HCI RX queue overflow", 0);
                return false;
            }
            notify |= !s->stats.rx_count;
            packet = &s->rx[(s->rx_head + s->stats.rx_count) %
                            MIBAND_SERIAL_RX_PACKETS];
            packet->epoch = s->stats.epoch;
            packet->sequence = ++s->stats.rx_packets;
            packet->received_ns = now;
            packet->length = wanted;
            memcpy(packet->data, s->partial, wanted);
            s->stats.rx_count++;
            s->stats.rx_bytes_queued += wanted;
            if (s->stats.rx_count > s->stats.rx_high_count) {
                s->stats.rx_high_count = s->stats.rx_count;
            }
            if (s->stats.rx_bytes_queued > s->stats.rx_high_bytes) {
                s->stats.rx_high_bytes = s->stats.rx_bytes_queued;
            }
            pthread_mutex_unlock(&s->lock);
            s->partial_len = 0;
        }
    }
    if (notify) {
        s->notify(s->opaque);
    }
    return true;
}

static bool serial_read_batch(MiBandBtSerial *s)
{
    uint8_t input[READ_SIZE];
    for (unsigned batch = 0; batch < IO_BATCH; batch++) {
        ssize_t n = read(s->fd, input, sizeof(input));
#ifdef _WIN32
        if (n == 0) { n = -1; errno = EAGAIN; }
#endif
        int saved_errno = errno;
        uint64_t now = serial_now();
        pthread_mutex_lock(&s->lock);
        s->stats.read_calls++;
        if (n > 0) {
            uint64_t gap = now - s->stats.last_read_ns;
            if (s->stats.last_read_ns && gap > s->stats.max_read_gap_ns) {
                s->stats.max_read_gap_ns = gap;
            }
            s->stats.last_read_ns = now;
            s->stats.rx_bytes += n;
        } else if (n < 0 && (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK)) {
            s->stats.read_eagain++;
        } else if (n < 0 && saved_errno == EINTR) {
            s->stats.read_eintr++;
        }
        bool stop = s->stop || s->stats.failed;
        pthread_mutex_unlock(&s->lock);
        if (stop) {
            return false;
        }
        if (n < 0 && saved_errno == EINTR) {
            continue;
        }
        if (n < 0 && (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK)) {
            return true;
        }
        if (n <= 0) {
            serial_fail(s, "physical serial disconnected during read",
                        n < 0 ? saved_errno : 0);
            return false;
        }
        serial_trace(s, 0, input, n, now);
        if (!serial_parse(s, input, n, now)) {
            return false;
        }
    }
    return true;
}

static bool serial_write_batch(MiBandBtSerial *s)
{
    for (unsigned batch = 0; batch < IO_BATCH; batch++) {
        unsigned head, length;
        ssize_t n;
        pthread_mutex_lock(&s->lock);
        if (s->stop || s->stats.failed || !s->stats.tx_count ||
            (s->rate && serial_now() < s->tx_next_ns)) {
            pthread_mutex_unlock(&s->lock);
            return true;
        }
        head = s->tx_head;
        length = s->tx_len[head] - s->tx_offset;
        if (s->rate && length > 64) {
            length = 64;
        }
        /* Only this thread removes entries. The producer cannot reuse head
         * before count is decremented after the syscall. */
        pthread_mutex_unlock(&s->lock);
        n = write(s->fd, s->tx[head] + s->tx_offset, length);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pthread_mutex_lock(&s->lock);
            s->stats.write_eagain++;
            pthread_mutex_unlock(&s->lock);
            return true;
        }
        if (n <= 0) {
            serial_fail(s, "physical serial write failed", n < 0 ? errno : 0);
            return false;
        }
        uint64_t now = serial_now();
        serial_trace(s, 1, s->tx[head] + s->tx_offset, n, now);
        pthread_mutex_lock(&s->lock);
        s->stats.tx_bytes += n;
        s->tx_offset += n;
        if (s->rate) {
            s->tx_next_ns = now + ((uint64_t)n * 1000000000 + s->rate - 1) /
                                 s->rate;
        }
        if (s->tx_offset == s->tx_len[head]) {
            s->tx_offset = 0;
            s->tx_head = (head + 1) % MIBAND_SERIAL_TX_PACKETS;
            s->stats.tx_count--;
            s->stats.tx_packets++;
        }
        pthread_mutex_unlock(&s->lock);
    }
    return true;
}

static void *serial_io_thread(void *opaque)
{
    MiBandBtSerial *s = opaque;
    #ifdef __APPLE__
    pthread_setname_np("miband-hci-io");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "miband-hci-io");
#endif
    for (;;) {
        struct pollfd pfd[2] = {{s->fd, POLLIN, 0}, {s->wake_fd, POLLIN, 0}};
        int timeout = -1;
        pthread_mutex_lock(&s->lock);
        if (s->stop || s->stats.failed) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        if (s->stats.tx_count) {
            uint64_t now = serial_now();
            if (s->rate && now < s->tx_next_ns) {
                uint64_t ms = (s->tx_next_ns - now + 999999) / 1000000;
                timeout = ms > INT_MAX ? INT_MAX : ms;
            } else {
                pfd[0].events |= POLLOUT;
            }
        }
        pthread_mutex_unlock(&s->lock);
#ifdef _WIN32
        int result = 1;
        DWORD errors;
        COMSTAT status;
        if (!ClearCommError((HANDLE)_get_osfhandle(s->fd), &errors, &status)) {
            pfd[0].revents = POLLERR;
        } else {
            if (status.cbInQue) pfd[0].revents |= POLLIN;
            if (pfd[0].events & POLLOUT) pfd[0].revents |= POLLOUT;
            DWORD wait = pfd[0].revents ? 0 : (timeout < 0 || timeout > 2 ? 2 : timeout);
            if (WaitForSingleObject(s->wake_event, wait) == WAIT_OBJECT_0) pfd[1].revents = POLLIN;
        }
#else
        int result = poll(pfd, 2, timeout);
#endif
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            serial_fail(s, "serial poll failed", errno);
            break;
        }
        if (pfd[1].revents & POLLIN) {
#ifdef _WIN32
            ResetEvent(s->wake_event);
#else
            uint64_t value;
            while (read(s->wake_fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
#endif
        }
        /* HUP is terminal even if a final fragment was buffered. Never report
         * the remaining stream as a successfully completed physical session. */
        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            serial_fail(s, "physical serial poll hangup/error", 0);
            break;
        }
        if ((pfd[0].revents & POLLIN) && !serial_read_batch(s)) {
            break;
        }
        if (!serial_write_batch(s)) {
            break;
        }
    }
    close(s->fd);                 /* The only close after ownership transfer. */
    return NULL;
}

MiBandBtSerial *miband_serial_start(int fd, unsigned rate, const char *trace_path,
                                  void (*notify)(void *), void *opaque,
                                  char *error, size_t error_size)
{
    MiBandBtSerial *s = calloc(1, sizeof(*s));
#ifndef _WIN32
    sigset_t blocked, previous;
#endif
    int rc;
    if (!s) {
        snprintf(error, error_size, "allocating HCI transport failed");
        return NULL;
    }
    s->fd = fd;
    s->trace_fd = -1;
#ifdef _WIN32
    s->wake_fd = s->wake_write = -1;
    s->wake_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!s->wake_event) { free(s); return NULL; }
#elif defined(__linux__)
    s->wake_fd = s->wake_write = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (s->wake_fd < 0) { free(s); return NULL; }
#else
    int wake_pipe[2];
    if (pipe(wake_pipe)) { free(s); return NULL; }
    s->wake_fd = wake_pipe[0]; s->wake_write = wake_pipe[1];
    fcntl(s->wake_fd, F_SETFL, O_NONBLOCK);
    fcntl(s->wake_write, F_SETFL, O_NONBLOCK);
    fcntl(s->wake_fd, F_SETFD, FD_CLOEXEC);
    fcntl(s->wake_write, F_SETFD, FD_CLOEXEC);
#endif
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->trace_cond, NULL);
    s->rate = rate;
    s->notify = notify;
    s->opaque = opaque;
    pthread_mutex_lock(&epoch_lock);
    s->stats.epoch = ++next_epoch;
    pthread_mutex_unlock(&epoch_lock);
    /* Workers inherit blocked signals. Restore the caller's mask immediately
     * after creating the threads, as QemuThread does for auxiliary threads. */
#ifndef _WIN32
    sigfillset(&blocked);
    pthread_sigmask(SIG_SETMASK, &blocked, &previous);
#endif
    if (trace_path && trace_path[0]) {
        s->trace_fd = open(trace_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC
#ifdef _WIN32
                           | O_BINARY
#endif
                           , 0600);
        if (s->trace_fd < 0) {
            snprintf(error, error_size, "opening new HCI trace failed: %s", strerror(errno));
            goto fail;
        }
        rc = pthread_create(&s->trace_thread, NULL, serial_trace_thread, s);
        if (rc) {
            snprintf(error, error_size, "starting HCI trace thread failed: %s", strerror(rc));
            close(s->trace_fd);
            goto fail;
        }
        s->trace_started = true;
    }
    rc = pthread_create(&s->io_thread, NULL, serial_io_thread, s);
    if (rc) {
        snprintf(error, error_size, "starting HCI IO thread failed: %s", strerror(rc));
        if (s->trace_started) {
            pthread_mutex_lock(&s->lock);
            s->trace_stop = true;
            pthread_cond_signal(&s->trace_cond);
            pthread_mutex_unlock(&s->lock);
            pthread_join(s->trace_thread, NULL);
        }
        goto fail;
    }
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
#endif
    return s;
fail:
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
#endif
#ifdef _WIN32
    CloseHandle(s->wake_event);
#else
    close(s->wake_fd);
    if (s->wake_write != s->wake_fd) close(s->wake_write);
#endif
    pthread_cond_destroy(&s->trace_cond);
    pthread_mutex_destroy(&s->lock);
    free(s);
    return NULL;
}

void *miband_serial_opaque(MiBandBtSerial *s)
{
    return s->opaque;
}

bool miband_serial_send(MiBandBtSerial *s, const uint8_t *data, unsigned length)
{
    bool ok;
    pthread_mutex_lock(&s->lock);
    ok = !s->stop && !s->stats.failed && length &&
         length <= MIBAND_SERIAL_PACKET_MAX &&
         s->stats.tx_count < MIBAND_SERIAL_TX_PACKETS;
    if (ok) {
        unsigned tail = (s->tx_head + s->stats.tx_count) % MIBAND_SERIAL_TX_PACKETS;
        memcpy(s->tx[tail], data, length);
        s->tx_len[tail] = length;
        s->stats.tx_count++;
        if (s->stats.tx_count > s->stats.tx_high_count) {
            s->stats.tx_high_count = s->stats.tx_count;
        }
    }
    pthread_mutex_unlock(&s->lock);
    if (!ok) {
        serial_fail(s, "physical HCI TX queue overflow or invalid submission", 0);
    } else {
        serial_wake(s);
    }
    return ok;
}

bool miband_serial_peek(MiBandBtSerial *s, MiBandSerialPacket *packet)
{
    bool ok;
    pthread_mutex_lock(&s->lock);
    ok = s->stats.rx_count && !s->stats.failed && !s->stop;
    if (ok) {
        *packet = s->rx[s->rx_head];
    }
    pthread_mutex_unlock(&s->lock);
    return ok;
}

void miband_serial_consume(MiBandBtSerial *s)
{
    pthread_mutex_lock(&s->lock);
    if (s->stats.rx_count) {
        s->stats.rx_bytes_queued -= s->rx[s->rx_head].length;
        s->stats.rx_count--;
        s->rx_head = (s->rx_head + 1) % MIBAND_SERIAL_RX_PACKETS;
    }
    pthread_mutex_unlock(&s->lock);
}

void miband_serial_stats(MiBandBtSerial *s, MiBandSerialStats *stats)
{
    pthread_mutex_lock(&s->lock);
    *stats = s->stats;
    stats->queue_oldest_ns = stats->rx_count ?
        serial_now() - s->rx[s->rx_head].received_ns : 0;
    pthread_mutex_unlock(&s->lock);
}

void miband_serial_stop(MiBandBtSerial *s)
{
    pthread_mutex_lock(&s->lock);
    s->stop = true;
    pthread_mutex_unlock(&s->lock);
    serial_wake(s);
    pthread_join(s->io_thread, NULL);
    if (s->trace_started) {
        pthread_mutex_lock(&s->lock);
        s->trace_stop = true;
        pthread_cond_signal(&s->trace_cond);
        pthread_mutex_unlock(&s->lock);
        pthread_join(s->trace_thread, NULL);
    }
#ifdef _WIN32
    CloseHandle(s->wake_event);
#else
    close(s->wake_fd);
    if (s->wake_write != s->wake_fd) close(s->wake_write);
#endif
    pthread_cond_destroy(&s->trace_cond);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
