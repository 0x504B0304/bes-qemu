#include "qemu/osdep.h"
#include "bes_host_file.h"
#include "qemu/log.h"
#include "hw/core/cpu.h"
#include "qemu/timer.h"
#include "sysemu/reset.h"
#include "sysemu/rtc.h"
#include "sysemu/sysemu.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include <zlib.h>
#include "best1502p_ispi.h"

#define ISPI_DATA 0x08u
#define ISPI_CONTROL 0x04u
#define ISPI_FIFO_STATUS 0x0cu
#define ISPI_TX_ROOM 0x02u
#define ISPI_RX_READY 0x04u
#define ISPI_READ_CMD (1u << 26)
#define ISPI_COMMAND_SHIFT 2
#define ISPI_CS_SHIFT 7
#define ISPI_CS_MASK 0xf
#define ISPI_PAGE_BASE 0xa000
#define ISPI_PMU_CS 3

/* reference calendar_get/set use CS3 page1, BCD fields and LOAD bit0.
 * This is a polled calendar, not an alarm/IRQ implementation. */
#define RTC_PAGE 1
#define RTC_CONTROL 0x0e
#define RTC_LOAD_TIME 0x0f
#define RTC_LOAD_HOUR 0x10
#define RTC_LOAD_DATE 0x11
#define RTC_LOAD_YEAR 0x12
#define RTC_READ_TIME 0x13
#define RTC_READ_HOUR 0x14
#define RTC_READ_DATE 0x15
#define RTC_READ_YEAR 0x16

static unsigned rtc_bcd(unsigned value)
{
    return (value / 10) * 16 + value % 10;
}

static unsigned rtc_decode(unsigned value)
{
    return (value & 15) > 9 || (value >> 4) > 9 ? UINT_MAX :
           (value >> 4) * 10 + (value & 15);
}

static int64_t rtc_now_ns(Best1502pIspi *ispi)
{
    return ispi->rtc_base_seconds * NANOSECONDS_PER_SECOND +
           qemu_clock_get_ns(rtc_clock) - ispi->rtc_start_ns;
}

static unsigned rtc_week_at(Best1502pIspi *ispi, int64_t seconds)
{
    int64_t days = seconds / 86400 - ispi->rtc_base_seconds / 86400;
    return ((ispi->rtc_base_week - 1 + days) % 7 + 7) % 7 + 1;
}

static void rtc_store_error(const char *message)
{
    error_report("best1502p RTC state: %s (errno=%d)", message, errno);
    exit(EXIT_FAILURE);
}

/* Small atomic sidecar, bound to the same images and lock as the NAND journal.
 * No new MBFLASH1 record type: existing journals remain byte compatible. */
static void rtc_save(Best1502pIspi *ispi)
{
    uint8_t record[68] = { 0 };
    int64_t now = rtc_now_ns(ispi);
    g_autofree char *temporary = NULL;
    g_autofree char *parent = NULL;
    int fd;
    size_t written = 0;

    if (!ispi->rtc_state_path) {
        return;
    }
    memcpy(record, "MBRTC001", 8);
    memcpy(record + 8, ispi->rtc_image_digest, 32);
    stq_le_p(record + 40, now);
    stq_le_p(record + 48, g_get_real_time());
    stl_le_p(record + 56, rtc_week_at(ispi, now / NANOSECONDS_PER_SECOND));
    stl_le_p(record + 60, rtc_clock);
    stl_le_p(record + 64, crc32(0, record, 64));
    temporary = g_strconcat(ispi->rtc_state_path, ".XXXXXX", NULL);
    fd = g_mkstemp_full(temporary, O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        rtc_store_error("cannot create checkpoint");
    }
    while (written < sizeof(record)) {
        ssize_t count = write(fd, record + written, sizeof(record) - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            rtc_store_error("checkpoint write failed");
        }
        written += count;
    }
    if (bes_data_sync(fd) || close(fd) || bes_replace(temporary, ispi->rtc_state_path)) {
        rtc_store_error("checkpoint commit failed");
    }
    parent = g_path_get_dirname(ispi->rtc_state_path);
    if (bes_dir_sync(parent)) {
        rtc_store_error("checkpoint directory sync failed");
    }

}

static void rtc_checkpoint(void *opaque)
{
    Best1502pIspi *ispi = opaque;
    rtc_save(ispi);
    timer_mod(ispi->rtc_checkpoint,
              qemu_clock_get_ns(rtc_clock) + NANOSECONDS_PER_SECOND);
}

static void rtc_exit(Notifier *notifier, void *unused)
{
    Best1502pIspi *ispi = container_of(notifier, Best1502pIspi, rtc_exit);
    rtc_save(ispi);
}

void best1502p_ispi_open_rtc(Best1502pIspi *ispi, const char *flash_path,
                           const uint8_t digest[32])
{
    uint8_t record[69];
    int fd;
    ssize_t count;

    ispi->rtc_state_path = g_strconcat(flash_path, ".rtc", NULL);
    memcpy(ispi->rtc_image_digest, digest, 32);
    fd = open(ispi->rtc_state_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 && errno != ENOENT) {
        rtc_store_error("cannot open saved calendar");
    }
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
            rtc_store_error("not a regular calendar file");
        }
        do {
            count = read(fd, record, sizeof(record));
        } while (count < 0 && errno == EINTR);
        close(fd);
        if (count != 68 || memcmp(record, "MBRTC001", 8) ||
            memcmp(record + 8, digest, 32) ||
            (uint32_t)ldl_le_p(record + 64) != (uint32_t)crc32(0, record, 64) ||
            ldl_le_p(record + 60) != rtc_clock ||
            ldl_le_p(record + 56) < 1 || ldl_le_p(record + 56) > 7 ||
            ldq_le_p(record + 40) > INT64_MAX / 2 ||
            ldq_le_p(record + 48) > INT64_MAX / 1000) {
            rtc_store_error("invalid calendar, image binding or clock policy");
        }
        int64_t epoch = ldq_le_p(record + 40);
        ispi->rtc_base_seconds = epoch / NANOSECONDS_PER_SECOND;
        ispi->rtc_base_week = ldl_le_p(record + 56);
        ispi->rtc_start_ns = qemu_clock_get_ns(rtc_clock) -
                            epoch % NANOSECONDS_PER_SECOND;
        /* VM time freezes while the process is absent. Host/rt calendars
         * continue from a wall-time anchor, never backwards on host rollback. */
        if (rtc_clock != QEMU_CLOCK_VIRTUAL) {
            int64_t elapsed = MAX(INT64_C(0), g_get_real_time() -
                                  (int64_t)ldq_le_p(record + 48));
            ispi->rtc_start_ns -= elapsed * 1000;
        }
    }
    rtc_save(ispi);
    ispi->rtc_checkpoint = timer_new_ns(rtc_clock, rtc_checkpoint, ispi);
    timer_mod(ispi->rtc_checkpoint,
              qemu_clock_get_ns(rtc_clock) + NANOSECONDS_PER_SECOND);
    ispi->rtc_exit.notify = rtc_exit;
    qemu_add_exit_notifier(&ispi->rtc_exit);
}

static uint16_t rtc_read(Best1502pIspi *ispi, unsigned reg)
{
    time_t seconds = rtc_now_ns(ispi) / NANOSECONDS_PER_SECOND;
    struct tm calendar;
    unsigned week = rtc_week_at(ispi, seconds);

    gmtime_r(&seconds, &calendar);
    switch (reg) {
    case RTC_READ_TIME:
        return rtc_bcd(calendar.tm_min) << 8 | rtc_bcd(calendar.tm_sec);
    case RTC_READ_HOUR:
        return rtc_bcd(calendar.tm_hour);
    case RTC_READ_DATE:
        return week << 13 | rtc_bcd(calendar.tm_mon + 1) << 8 |
               rtc_bcd(calendar.tm_mday);
    case RTC_READ_YEAR:
        return rtc_bcd((calendar.tm_year + 1900) % 100);
    default:
        g_assert_not_reached();
    }
}

static void rtc_load(Best1502pIspi *ispi)
{
    uint16_t *r = ispi->device_regs[ISPI_PMU_CS][RTC_PAGE];
    unsigned second = rtc_decode(r[RTC_LOAD_TIME] & 0x7f);
    unsigned minute = rtc_decode((r[RTC_LOAD_TIME] >> 8) & 0x7f);
    unsigned hour = rtc_decode(r[RTC_LOAD_HOUR] & 0x3f);
    unsigned day = rtc_decode(r[RTC_LOAD_DATE] & 0x3f);
    unsigned month = rtc_decode((r[RTC_LOAD_DATE] >> 8) & 0x1f);
    unsigned year = rtc_decode(r[RTC_LOAD_YEAR] & 0xff);
    unsigned week = r[RTC_LOAD_DATE] >> 13;
    struct tm calendar = { 0 }, normalized;
    time_t seconds;

    r[RTC_CONTROL] &= ~1;
    if (second > 59 || minute > 59 || hour > 23 || !day || day > 31 ||
        !month || month > 12 || year > 99 || !week || week > 7) {
        qemu_log_mask(LOG_GUEST_ERROR, "best1502p RTC invalid BCD load\n");
        return;
    }
    calendar.tm_year = year + 100;
    calendar.tm_mon = month - 1;
    calendar.tm_mday = day;
    calendar.tm_hour = hour;
    calendar.tm_min = minute;
    calendar.tm_sec = second;
    seconds = mktimegm(&calendar);
    gmtime_r(&seconds, &normalized);
    if (normalized.tm_year != calendar.tm_year ||
        normalized.tm_mon != calendar.tm_mon ||
        normalized.tm_mday != calendar.tm_mday) {
        qemu_log_mask(LOG_GUEST_ERROR, "best1502p RTC invalid date load\n");
        return;
    }
    ispi->rtc_base_seconds = seconds;
    ispi->rtc_start_ns = qemu_clock_get_ns(rtc_clock);
    ispi->rtc_base_week = week;
    ispi->device_regs[ISPI_PMU_CS][0][0x61] |= 1;
    rtc_save(ispi);
}

/* Qualified polling subset of the PMU GPADC, see docs/BEST1502P_GPADC.md.
 * No IRQ or key detector is synthesized. Sampling advances in virtual time,
 * not on reads, so repeated status reads cannot manufacture completions. */
static void gpadc_update(Best1502pIspi *ispi)
{
    uint16_t *r = ispi->device_regs[ISPI_PMU_CS][0];
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint16_t sampled = 0;

    if (!ispi->adc_deadline || now < ispi->adc_deadline) {
        return;
    }
    if (r[0x4f] & (1u << 5)) {
        for (unsigned ch = 0; ch < 7; ch++) {
            if ((r[0x1d] & (1u << ch)) && ispi->adc_input[ch] <= UINT16_MAX) {
                r[0x56 + ch] = ispi->adc_input[ch];
                sampled |= 1u << ch;
            }
        }
    }
    ispi->adc_pending |= sampled;
    if (sampled) {
        ispi->adc_pending |= 1u << 8;
    }
    if ((r[0x4f] & (1u << 5)) && (r[0x18] & (1u << 12))) {
        int64_t period = MAX((int64_t)r[0x1c] * 1024, 1024);
        ispi->adc_deadline += ((now - ispi->adc_deadline) / period + 1) * period;
    } else {
        ispi->adc_deadline = 0;
        r[0x4f] &= ~(1u << 5);
    }
}

static void best1502p_ispi_reset(void *opaque)
{
    Best1502pIspi *ispi = opaque;
    uint32_t inputs[8];

    memcpy(inputs, ispi->adc_input, sizeof(inputs));
    /* Everything from rtc_base_seconds onward belongs to the retained domain. */
    memset(ispi, 0, offsetof(Best1502pIspi, rtc_base_seconds));
    memcpy(ispi->adc_input, inputs, sizeof(inputs));
    ispi->device_regs[ISPI_PMU_CS][0][0] = 0xc0d0;
    ispi->device_regs[ISPI_PMU_CS][0][0x61] = 1;
}

static bool ispi_page_value(uint16_t value)
{
    unsigned page = (value - ISPI_PAGE_BASE) >> 4;

    return value >= ISPI_PAGE_BASE && page < BEST1502P_ISPI_PAGE_COUNT &&
           (value & 0xf) == 0;
}

static unsigned ispi_page_index(const Best1502pIspi *ispi)
{
    uint16_t page = ispi->page[ispi->chip_select];

    return ispi_page_value(page) ? (page - ISPI_PAGE_BASE) >> 4 : 0;
}

static void ispi_trace(Best1502pIspi *ispi, const char *kind, uint32_t value,
                       unsigned reg, unsigned page, uint16_t data)
{
    if (ispi->trace_events++ >= 8192) {
        return;
    }
    uint32_t pc = current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0;

    qemu_log_mask(LOG_UNIMP,
                  "best1502p ISPI %s cs=%u value=%08" PRIx32
                  " reg=%02x page=%u data=%04x pc=%08" PRIx32 "\n",
                  kind, ispi->chip_select, value, reg, page, data, pc);
}

void best1502p_ispi_init(Best1502pIspi *ispi)
{
    struct tm calendar;
    memset(ispi, 0, sizeof(*ispi));
    memset(ispi->adc_input, 0xff, sizeof(ispi->adc_input));
    /* reference's boot path compares the returned value shifted right by four
     * against 0xC0D (observed at 0x0c0c1e90), so its PMU identity is 0xC0D0.
     * This is intentionally separate from the BEST1503 value (0x1530). */
    ispi->device_regs[ISPI_PMU_CS][0][0] = 0xc0d0;
    qemu_get_timedate(&calendar, 0);
    ispi->rtc_base_seconds = mktimegm(&calendar);
    ispi->rtc_start_ns = qemu_clock_get_ns(rtc_clock);
    ispi->rtc_base_week = calendar.tm_wday + 1;
    ispi->device_regs[ISPI_PMU_CS][0][0x61] = 1;
    qemu_register_reset(best1502p_ispi_reset, ispi);
}

bool best1502p_ispi_read(Best1502pIspi *ispi, hwaddr offset,
                         unsigned size, uint64_t *result)
{
    if (size != sizeof(uint32_t) || offset >= BEST1502P_ISPI_SIZE ||
        (offset & 3)) {
        return false;
    }
    switch (offset) {
    case ISPI_DATA:
        *result = ispi->rx_data;
        ispi->rx_ready = false;
        return true;
    case ISPI_CONTROL:
        *result = ispi->control;
        return true;
    case ISPI_FIFO_STATUS:
        *result = ISPI_TX_ROOM | (ispi->rx_ready ? ISPI_RX_READY : 0);
        return true;
    default:
        *result = ispi->regs[offset >> 2];
        return true;
    }
}

bool best1502p_ispi_write(Best1502pIspi *ispi, hwaddr offset,
                          unsigned size, uint64_t value)
{
    uint32_t command = (uint32_t)value;
    unsigned reg;
    unsigned page;
    uint16_t data;

    if (size != sizeof(uint32_t) || offset >= BEST1502P_ISPI_SIZE ||
        (offset & 3)) {
        return false;
    }
    if (offset == ISPI_CONTROL) {
        ispi->control = command;
        ispi->chip_select = (command >> ISPI_CS_SHIFT) & ISPI_CS_MASK;
        ispi_trace(ispi, "control", command, 0, 0, 0);
        return true;
    }
    if (offset != ISPI_DATA) {
        ispi->regs[offset >> 2] = command;
        return true;
    }

    reg = (command >> (16 + ISPI_COMMAND_SHIFT)) & 0xff;
    page = ispi_page_index(ispi);
    gpadc_update(ispi);
    if (command & ISPI_READ_CMD) {
        data = ispi->device_regs[ispi->chip_select][page][reg];
        if (ispi->chip_select == ISPI_PMU_CS && page == RTC_PAGE &&
            reg >= RTC_READ_TIME && reg <= RTC_READ_YEAR) {
            data = rtc_read(ispi, reg);
        }
        if (ispi->chip_select == ISPI_PMU_CS && page == 0) {
            if (reg == 0x50) {
                data = ispi->adc_pending;
            } else if (reg == 0x51) {
                data = ispi->adc_pending & ispi->device_regs[3][0][0x1f] &
                       ispi->device_regs[3][0][0x20];
            }
        }
        if (ispi->chip_select == ISPI_PMU_CS && page == 0 && reg == 0) {
            data = 0xc0d0;
        }
        ispi->rx_data = (uint32_t)data << ISPI_COMMAND_SHIFT;
        ispi_trace(ispi, "read", command, reg, page, data);
    } else {
        data = (command >> ISPI_COMMAND_SHIFT) & 0xffff;
        if (reg == 0 && ispi_page_value(data)) {
            ispi->page[ispi->chip_select] = data;
        } else {
            if (ispi->chip_select == ISPI_PMU_CS && page == 0 && reg == 0x51) {
                ispi->adc_pending &= ~data;
            } else if (ispi->chip_select == ISPI_PMU_CS && page == RTC_PAGE &&
                       reg >= RTC_READ_TIME && reg <= RTC_READ_YEAR) {
                /* The calendar is committed through LOAD, not readback. */
            } else if (!(ispi->chip_select == ISPI_PMU_CS && page == 0 &&
                         (reg == 0x50 || (reg >= 0x56 && reg <= 0x5d)))) {
                ispi->device_regs[ispi->chip_select][page][reg] = data;
            }
            if (ispi->chip_select == ISPI_PMU_CS && page == RTC_PAGE &&
                reg == RTC_CONTROL && (data & 1)) {
                rtc_load(ispi);
            }
            if (ispi->chip_select == ISPI_PMU_CS && page == 0 && reg == 0x4f) {
                ispi->adc_deadline = (data & (1u << 5)) ?
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 125000 : 0;
            }
        }
        ispi->rx_data = 0;
        ispi_trace(ispi, "write", command, reg, page, data);
    }
    ispi->rx_ready = true;
    return true;
}
