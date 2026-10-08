/* Compile the production transport itself against PTYs; no QEMU or serial
 * device is opened. clang-23 -DMIBAND_SERIAL_STANDALONE ... -pthread -lutil */
#define _GNU_SOURCE
#include "miband_bt_serial.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pty.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

typedef struct Fixture {
    MiBandBtSerial *io;
    int master;
    int transferred_fd;
} Fixture;

static atomic_uint notifications;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void notified(void *unused)
{
    (void)unused;
    atomic_fetch_add(&notifications, 1);
}

static Fixture fixture(unsigned rate, const char *trace)
{
    Fixture f;
    struct termios term;
    char error[200];
    assert(openpty(&f.master, &f.transferred_fd, NULL, NULL, NULL) == 0);
    assert(tcgetattr(f.transferred_fd, &term) == 0);
    cfmakeraw(&term);
    term.c_cc[VMIN] = 1;
    term.c_cc[VTIME] = 0;
    assert(tcsetattr(f.transferred_fd, TCSANOW, &term) == 0);
    assert(fcntl(f.transferred_fd, F_SETFL, O_NONBLOCK) == 0);
    assert(fcntl(f.master, F_SETFL, O_NONBLOCK) == 0);
    f.io = miband_serial_start(f.transferred_fd, rate, trace, notified, NULL,
                               error, sizeof(error));
    if (!f.io) {
        fprintf(stderr, "transport start: %s\n", error);
        abort();
    }
    return f;
}

static void stop_fixture(Fixture *f)
{
    uint64_t start = now_ns();
    miband_serial_stop(f->io);
    assert(now_ns() - start < 1000000000);
    errno = 0;
    assert(fcntl(f->transferred_fd, F_GETFD) == -1 && errno == EBADF);
    if (f->master >= 0) {
        close(f->master);
    }
}

static void write_all(int fd, const uint8_t *data, unsigned length)
{
    uint64_t deadline = now_ns() + 3000000000;
    while (length) {
        assert(now_ns() < deadline);
        ssize_t n = write(fd, data, length);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd p = {fd, POLLOUT, 0};
            poll(&p, 1, 10);
            continue;
        }
        assert(n > 0);
        data += n;
        length -= n;
    }
}

static MiBandSerialStats wait_rx(Fixture *f, unsigned count, bool failed)
{
    MiBandSerialStats stats;
    uint64_t deadline = now_ns() + 3000000000;
    do {
        miband_serial_stats(f->io, &stats);
        if (stats.failed || stats.rx_count >= count) {
            break;
        }
        usleep(1000);
    } while (now_ns() < deadline);
    assert(stats.failed == failed);
    if (!failed) {
        assert(stats.rx_count >= count);
    }
    return stats;
}

static void test_fragment_and_pause(void)
{
    Fixture f = fixture(0, NULL);
    const uint8_t event[] = {4, 0x0e, 4, 1, 3, 0x0c, 0};
    MiBandSerialPacket packet;
    MiBandSerialStats stats;
    write_all(f.master, event, 2);
    usleep(10000);
    assert(!miband_serial_peek(f.io, &packet));
    write_all(f.master, event + 2, 1);
    write_all(f.master, event + 3, sizeof(event) - 3);
    wait_rx(&f, 1, false);
    assert(miband_serial_peek(f.io, &packet));
    assert(packet.sequence == 1 && packet.length == sizeof(event));
    assert(!memcmp(packet.data, event, sizeof(event)));
    uint64_t epoch = packet.epoch;
    miband_serial_consume(f.io);

    /* Simulate a stopped main consumer: IO must continue beyond the former
     * 32-packet AP queue, preserve all bytes, then resume in original order. */
    uint8_t burst[100 * 4];
    for (unsigned i = 0; i < 100; i++) {
        memcpy(burst + i * 4, (uint8_t[]){4, 0xff, 1, i}, 4);
    }
    write_all(f.master, burst, sizeof(burst));
    stats = wait_rx(&f, 100, false);
    assert(stats.rx_bytes_queued == sizeof(burst));
    usleep(10000);
    for (unsigned i = 0; i < 100; i++) {
        assert(miband_serial_peek(f.io, &packet));
        assert(packet.epoch == epoch && packet.sequence == i + 2);
        assert(packet.length == 4 && packet.data[3] == i);
        miband_serial_consume(f.io);
    }
    assert(!miband_serial_peek(f.io, &packet));
    stop_fixture(&f);
    puts("PASS split/merged H4, 100-packet paused-consumer backlog, FIFO resume");
}

static void test_tx_backpressure(void)
{
    Fixture f = fixture(0, NULL);
    uint8_t expected[32 * 2048], received[sizeof(expected)];
    MiBandSerialStats stats;
    for (unsigned i = 0; i < sizeof(expected); i++) {
        expected[i] = (i / 2048) ^ i;
    }
    for (unsigned i = 0; i < 32; i++) {
        assert(miband_serial_send(f.io, expected + i * 2048, 2048));
    }
    /* A batch can end on the partial write that fills the PTY, before a
     * further write has returned EAGAIN. Observe kernel backpressure too,
     * then wake the worker with RX so that it retries the blocked TX. */
    uint64_t backpressure_deadline = now_ns() + 3000000000;
    bool blocked = false;
    do {
        struct pollfd output = {f.transferred_fd, POLLOUT, 0};
        int ready = poll(&output, 1, 0);
        assert(ready >= 0 || errno == EINTR);
        miband_serial_stats(f.io, &stats);
        blocked = stats.tx_count > 0 && stats.tx_bytes > 0 &&
                  (stats.write_eagain > 0 ||
                   (ready == 0 && !(output.revents & POLLOUT)));
        if (stats.failed || blocked) {
            break;
        }
        usleep(1000);
    } while (now_ns() < backpressure_deadline);
    if (stats.failed || !blocked) {
        fprintf(stderr, "TX backpressure %s: failed=%d tx_count=%u "
                "write_eagain=%" PRIu64 " tx_packets=%" PRIu64
                " tx_bytes=%" PRIu64 " error=%s\n",
                stats.failed ? "transport failure" : "timeout after 3 s",
                stats.failed, stats.tx_count, stats.write_eagain,
                stats.tx_packets, stats.tx_bytes, stats.error);
    }
    assert(!stats.failed && blocked);
    /* A blocked transmitter must still drain the controller's RX direction. */
    write_all(f.master, (uint8_t[]){4, 0xff, 1, 0x5a}, 4);
    wait_rx(&f, 1, false);
    backpressure_deadline = now_ns() + 3000000000;
    do {
        miband_serial_stats(f.io, &stats);
        if (stats.failed || stats.write_eagain > 0) {
            break;
        }
        usleep(1000);
    } while (now_ns() < backpressure_deadline);
    assert(!stats.failed && stats.tx_count > 0 && stats.write_eagain > 0);
    unsigned have = 0;
    uint64_t deadline = now_ns() + 3000000000;
    while (have < sizeof(received)) {
        assert(now_ns() < deadline);
        ssize_t n = read(f.master, received + have, sizeof(received) - have);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            usleep(1000);
            continue;
        }
        assert(n > 0);
        have += n;
    }
    assert(!memcmp(expected, received, sizeof(expected)));
    do {
        miband_serial_stats(f.io, &stats);
    } while (stats.tx_count && now_ns() < deadline);
    assert(stats.tx_packets == 32 && stats.tx_bytes == sizeof(expected));
    stop_fixture(&f);
    puts("PASS TX EAGAIN/partial-write continuation, exact byte order, concurrent RX");
}

static void test_packet_limit(void)
{
    Fixture f = fixture(0, NULL);
    uint8_t burst[(MIBAND_SERIAL_RX_PACKETS + 1) * 3];
    for (unsigned i = 0; i < sizeof(burst); i += 3) {
        memcpy(burst + i, (uint8_t[]){4, 0xff, 0}, 3);
    }
    write_all(f.master, burst, sizeof(burst));
    MiBandSerialStats s = wait_rx(&f, UINT32_MAX, true);
    assert(s.rx_count == MIBAND_SERIAL_RX_PACKETS);
    assert(strstr(s.error, "overflow"));
    MiBandSerialPacket packet;
    assert(!miband_serial_peek(f.io, &packet));
    stop_fixture(&f);
    puts("PASS 1024-packet bound, explicit failure, no stale packet delivery");
}

static void test_byte_limit(void)
{
    Fixture f = fixture(0, NULL);
    uint8_t packet[2048] = {2, 1, 0, 0xfb, 7};
    for (unsigned i = 0; i < MIBAND_SERIAL_RX_BYTES / sizeof(packet); i++) {
        packet[5] = i;
        write_all(f.master, packet, sizeof(packet));
    }
    MiBandSerialStats s = wait_rx(&f, 256, false);
    assert(s.rx_bytes_queued == MIBAND_SERIAL_RX_BYTES);
    write_all(f.master, (uint8_t[]){4, 0xff, 0}, 3);
    s = wait_rx(&f, UINT32_MAX, true);
    assert(s.rx_bytes_queued == MIBAND_SERIAL_RX_BYTES && s.rx_count == 256);
    stop_fixture(&f);
    puts("PASS independent 512 KiB byte bound and fail-closed overflow");
}

static void test_corruption_and_epoch(void)
{
    Fixture f = fixture(0, NULL);
    MiBandSerialStats s;
    miband_serial_stats(f.io, &s);
    uint64_t previous_epoch = s.epoch;
    write_all(f.master, (uint8_t[]){0xff, 4, 0xff, 0}, 4);
    s = wait_rx(&f, UINT32_MAX, true);
    assert(s.rx_packets == 0 && strstr(s.error, "framing"));
    stop_fixture(&f);
    f = fixture(0, NULL);
    miband_serial_stats(f.io, &s);
    assert(s.epoch > previous_epoch && s.rx_packets == 0);
    write_all(f.master, (uint8_t[]){2, 1, 0, 0xff, 0x7f}, 5);
    s = wait_rx(&f, UINT32_MAX, true);
    assert(s.rx_packets == 0 && strstr(s.error, "framing"));
    stop_fixture(&f);
    f = fixture(0, NULL);
    close(f.master);
    f.master = -1;
    s = wait_rx(&f, UINT32_MAX, true);
    assert(strstr(s.error, "hangup") || strstr(s.error, "disconnected"));
    stop_fixture(&f);
    puts("PASS strict invalid/oversize H4, new epoch, physical hangup");
}

static void test_shutdown_and_trace(const char *directory)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/hci.bin", directory);
    Fixture f = fixture(0, path);
    uint8_t data[] = {4, 0xff, 1, 0x42};
    write_all(f.master, data, sizeof(data));
    wait_rx(&f, 1, false);
    assert(miband_serial_send(f.io, data, sizeof(data)));
    MiBandSerialStats s;
    uint64_t deadline = now_ns() + 3000000000;
    do {
        miband_serial_stats(f.io, &s);
        usleep(1000);
    } while (!s.tx_packets && now_ns() < deadline);
    assert(s.tx_packets == 1 && !s.trace_dropped && !s.trace_errors);
    stop_fixture(&f);
    int fd = open(path, O_RDONLY);
    uint8_t trace[128];
    ssize_t len = read(fd, trace, sizeof(trace));
    close(fd);
    assert(len == 8 + 2 * (32 + sizeof(data)));
    assert(!memcmp(trace, "MBHCI001", 8));
    assert(trace[8 + 28] == 0 && trace[8 + 32 + 4 + 28] == 1);
    assert(!memcmp(trace + 8 + 32, data, 4));
    assert(!memcmp(trace + 8 + 32 + 4 + 32, data, 4));
    /* Stopping an idle poll or a partially assembled H4 header cannot wait
     * for another serial byte, and shutdown closes the transferred fd. */
    for (unsigned i = 0; i < 20; i++) {
        f = fixture(0, NULL);
        if (i & 1) {
            write_all(f.master, (uint8_t[]){2, 1}, 2);
        }
        stop_fixture(&f);
    }
    puts("PASS asynchronous binary RX/TX trace, idle/partial-header shutdown");
}

static uint64_t le64(const uint8_t *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) {
        value |= (uint64_t)p[i] << (8 * i);
    }
    return value;
}

static void test_pacing(const char *directory)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/pacing.bin", directory);
    Fixture f = fixture(12800, path);
    uint8_t packet[200] = {2, 1, 0, 195, 0};
    MiBandSerialStats s;
    assert(miband_serial_send(f.io, packet, sizeof(packet)));
    write_all(f.master, (uint8_t[]){4, 0xff, 0}, 3);
    wait_rx(&f, 1, false);
    uint64_t deadline = now_ns() + 3000000000;
    do {
        miband_serial_stats(f.io, &s);
        usleep(1000);
    } while (!s.tx_packets && now_ns() < deadline);
    assert(s.tx_packets == 1);
    stop_fixture(&f);
    int fd = open(path, O_RDONLY);
    uint8_t trace[1024];
    ssize_t size = read(fd, trace, sizeof(trace));
    close(fd);
    uint64_t previous = 0;
    unsigned previous_length = 0, total = 0, writes = 0;
    for (unsigned offset = 8; offset < (unsigned)size; ) {
        assert(offset + 32 <= (unsigned)size);
        unsigned length = trace[offset + 24] | trace[offset + 25] << 8;
        if (trace[offset + 28] == 1) {
            uint64_t ns = le64(trace + offset);
            assert(length > 0 && length <= 64);
            if (previous) {
                assert(ns - previous >=
                       ((uint64_t)previous_length * 1000000000 + 12799) / 12800);
            }
            assert(!memcmp(trace + offset + 32, packet + total, length));
            previous = ns;
            previous_length = length;
            total += length;
            writes++;
        }
        offset += 32 + length;
    }
    assert(total == sizeof(packet) && writes == 4);
    /* A pacing deadline far in the future must remain interruptible by stop. */
    f = fixture(1, NULL);
    assert(miband_serial_send(f.io, packet, sizeof(packet)));
    usleep(10000);
    stop_fixture(&f);
    puts("PASS optional TX pacing from actual binary trace and pacing-stop wakeup");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_fragment_and_pause();
    test_tx_backpressure();
    test_packet_limit();
    test_byte_limit();
    test_corruption_and_epoch();
    test_shutdown_and_trace(argv[1]);
    test_pacing(argv[1]);
    printf("PASS all transport PTY tests; notifications=%u\n",
           atomic_load(&notifications));
    return 0;
}
