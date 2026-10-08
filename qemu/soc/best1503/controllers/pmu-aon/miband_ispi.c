#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/cutils.h"
#include "hw/core/cpu.h"
#include "sysemu/rtc.h"
#include "sysemu/sysemu.h"
#include "miband_peripherals.h"
#include "miband_charger.h"

#define ISPI_BASE        0x40009000
#define ISPI_DATA        0x08
#define ISPI_CONTROL     0x04
#define ISPI_FIFO_STATUS 0x0c
#define ISPI_TX_ROOM     0x02
#define ISPI_RX_READY    0x04
#define ISPI_READ_CMD    (1U << 26)
#define ISPI_CMD_SHIFT   2
#define ISPI_CS_SHIFT    7
#define ISPI_CS_MASK     0xf
#define ISPI_PMU_CS      3
#define ISPI_PAGE_BASE   0xa000
#define ISPI_PAGE_COUNT  MIBAND_ISPI_PAGE_COUNT
#define RTC_PAGE        1
#define RTC_CONTROL     0x0e
#define RTC_LOAD_TIME   0x0f
#define RTC_LOAD_HOUR   0x10
#define RTC_LOAD_DATE   0x11
#define RTC_LOAD_YEAR   0x12
#define RTC_READ_TIME   0x13
#define RTC_READ_HOUR   0x14
#define RTC_READ_DATE   0x15
#define RTC_READ_YEAR   0x16
#define RTC_LOAD        0x0001
#define RTC_VALID_REG   0x61
#define RTC_VALID       0x0001

/* Registers confirmed by the best2007p GPADC implementation and observed in
 * this AP's PMU transaction trace.  The model is intentionally narrow: it
 * supplies a ready sample for enabled channels. reference unified IRQ handling
 * confirms masked status at 0x51, channel mask at 0x1f and slot-0 enable
 * at 0x40 bit 13. PMU power sequencing is still outside this subset. */
#define GPADC_REG_CH_EN      0x1d
#define GPADC_REG_INTERVAL_ENABLE 0x18
#define GPADC_REG_INTERVAL   0x1c
#define GPADC_INTERVAL_MODE  (1u << 12)
#define GPADC_REG_INT_EN     0x20
#define GPADC_REG_IRQ_MASK   0x1f
#define GPADC_UNIFIED_ENABLE (1u << 13)
#define GPADC_REG_START      0x4f
#define GPADC_REG_RAW_STATUS 0x50
#define GPADC_REG_STATUS_CLR 0x51
#define GPADC_REG_CH_DATA    0x56
#define GPADC_START          (1U << 5)
#define PMU_CHARGER_ENABLE   0x05
#define PMU_UNIFIED_ENABLE   0x40
#define PMU_CHARGER_STATUS   0x5e
#define PMU_CHARGER_AC_ON    (1u << 2)
#define PMU_CHARGER_IN_IRQ   (1u << 3)
#define PMU_CHARGER_OUT_IRQ  (1u << 4)
#define PMU_CHARGER_IRQS     (PMU_CHARGER_IN_IRQ | PMU_CHARGER_OUT_IRQ)
#define PMU_CHARGER_IN_ENABLE  (1u << 11)
#define PMU_CHARGER_OUT_ENABLE (1u << 12)
#define PMU_CHARGER_UNIFIED_ENABLE (1u << 12)

static void ispi_pmu_update_irq(MiBandIspi *ispi)
{
    uint16_t *regs = ispi->regs[ISPI_PMU_CS][0];
    uint16_t status = regs[PMU_CHARGER_STATUS];
    uint16_t enabled = regs[PMU_CHARGER_ENABLE];
    bool pending = ((status & PMU_CHARGER_IN_IRQ) &&
                    (enabled & PMU_CHARGER_IN_ENABLE)) ||
                   ((status & PMU_CHARGER_OUT_IRQ) &&
                    (enabled & PMU_CHARGER_OUT_ENABLE));

    bool charger_pending = ispi->charger && ispi->charger->enabled &&
        (regs[PMU_UNIFIED_ENABLE] & PMU_CHARGER_UNIFIED_ENABLE) && pending;
    bool adc_pending = ispi->gpadc_model &&
        (regs[PMU_UNIFIED_ENABLE] & GPADC_UNIFIED_ENABLE) &&
        (ispi->gpadc_valid & regs[GPADC_REG_IRQ_MASK] & 0x1fff);

    if (ispi->pmu_irq) {
        qemu_set_irq(ispi->pmu_irq, charger_pending || adc_pending);
    }
}

static void ispi_gpadc_schedule(MiBandIspi *ispi)
{
    uint16_t *regs = ispi->regs[ISPI_PMU_CS][0];

    timer_del(ispi->gpadc_timer);
    if (ispi->gpadc_model && (regs[GPADC_REG_START] & GPADC_START) &&
        (regs[GPADC_REG_INTERVAL_ENABLE] & GPADC_INTERVAL_MODE) &&
        (regs[GPADC_REG_CH_EN] & 0x7f)) {
        /* The driver programs atp_us * 1000 / 1024 (122 for 125 us).
         * Each unit is 1024 ns; zero uses one unit to avoid a busy loop. */
        int64_t period_ns = (int64_t)MAX(1, regs[GPADC_REG_INTERVAL]) * 1024;
        timer_mod(ispi->gpadc_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + period_ns);
    }
}

static void ispi_gpadc_sample(MiBandIspi *ispi)
{
    uint16_t *regs = ispi->regs[ISPI_PMU_CS][0];

    ispi->gpadc_valid |= regs[GPADC_REG_CH_EN] & regs[GPADC_REG_INT_EN] & 0x7f;
    if (ispi->gpadc_trace_events++ < 256) {
        qemu_log_mask(LOG_UNIMP,
                      "miband PMU GPADC sample ready channels=%04x "
                      "mask=%04x unified=%04x interval=%u\n",
                      ispi->gpadc_valid, regs[GPADC_REG_IRQ_MASK],
                      regs[PMU_UNIFIED_ENABLE], regs[GPADC_REG_INTERVAL]);
    }
    ispi_pmu_update_irq(ispi);
}

static void ispi_gpadc_tick(void *opaque)
{
    MiBandIspi *ispi = opaque;

    ispi_gpadc_sample(ispi);
    ispi_gpadc_schedule(ispi);
}

void miband_ispi_shutdown(MiBandIspi *ispi)
{
    timer_free(ispi->gpadc_timer);
    ispi->gpadc_timer = NULL;
}

void miband_ispi_gpadc_reset(MiBandIspi *ispi)
{
    timer_del(ispi->gpadc_timer);
    ispi->gpadc_valid = 0;
    ispi->regs[ISPI_PMU_CS][0][GPADC_REG_START] &= ~GPADC_START;
    ispi_pmu_update_irq(ispi);
}

void miband_ispi_connect_pmu_irq(MiBandIspi *ispi, qemu_irq irq)
{
    ispi->pmu_irq = irq;
    ispi_pmu_update_irq(ispi);
}

void miband_ispi_charger_changed(MiBandIspi *ispi, bool connected)
{
    /* C0E62EC snapshots selector 0x305e and writes it back to acknowledge
     * bits 3/4 before delivering unified callback slot 2. AC_ON is read-only.
     * IRQ42 is shared with other PMU sources; only this confirmed source is
     * modeled here. */
    ispi->regs[ISPI_PMU_CS][0][PMU_CHARGER_STATUS] |= connected ?
        PMU_CHARGER_IN_IRQ : PMU_CHARGER_OUT_IRQ;
    ispi_pmu_update_irq(ispi);
    qemu_log_mask(LOG_UNIMP, "miband PMU charger edge connected=%u"
                  " latch=%04x enable=%04x unified=%04x irq=42\n", connected,
                  ispi->regs[ISPI_PMU_CS][0][PMU_CHARGER_STATUS],
                  ispi->regs[ISPI_PMU_CS][0][PMU_CHARGER_ENABLE],
                  ispi->regs[ISPI_PMU_CS][0][PMU_UNIFIED_ENABLE]);
}

void miband_ispi_charger_reset(MiBandIspi *ispi)
{
    ispi->regs[ISPI_PMU_CS][0][PMU_CHARGER_STATUS] &= ~PMU_CHARGER_IRQS;
    ispi_pmu_update_irq(ispi);
}

static bool ispi_page_value(uint16_t value)
{
    unsigned page = (value - ISPI_PAGE_BASE) >> 4;

    return value >= ISPI_PAGE_BASE && page < ISPI_PAGE_COUNT &&
           (value & 0xf) == 0;
}

static unsigned ispi_page_index(const MiBandIspi *ispi)
{
    uint16_t page = ispi->page[ispi->chip_select];

    return ispi_page_value(page) ? (page - ISPI_PAGE_BASE) >> 4 : 0;
}

static uint16_t rtc_bcd(unsigned value)
{
    return ((value / 10) << 4) | (value % 10);
}

static unsigned rtc_from_bcd(uint16_t value)
{
    if ((value & 0xf) > 9 || (value >> 4) > 9) {
        return UINT_MAX;
    }
    return (value >> 4) * 10 + (value & 0xf);
}

static const char *ispi_rtc_clock_name(void)
{
    switch (rtc_clock) {
    case QEMU_CLOCK_HOST:
        return "host";
    case QEMU_CLOCK_REALTIME:
        return "rt";
    default:
        return "vm";
    }
}

static uint16_t ispi_rtc_read(MiBandIspi *ispi, unsigned reg)
{
    int64_t virtual_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t clock_ns = qemu_clock_get_ns(rtc_clock);
    time_t seconds = ispi->rtc_base_seconds +
                     (clock_ns - ispi->rtc_start_ns) / NANOSECONDS_PER_SECOND;
    struct tm calendar;
    uint16_t data;
    unsigned week = (ispi->rtc_base_week - 1 +
                     (seconds / 86400 - ispi->rtc_base_seconds / 86400)) % 7 + 1;

    gmtime_r(&seconds, &calendar);
    switch (reg) {
    case RTC_READ_TIME:
        data = (rtc_bcd(calendar.tm_min) << 8) | rtc_bcd(calendar.tm_sec);
        break;
    case RTC_READ_HOUR:
        data = rtc_bcd(calendar.tm_hour);
        break;
    case RTC_READ_DATE:
        data = (week << 13) |
               (rtc_bcd(calendar.tm_mon + 1) << 8) | rtc_bcd(calendar.tm_mday);
        break;
    case RTC_READ_YEAR:
        data = rtc_bcd((calendar.tm_year + 1900) % 100);
        break;
    default:
        g_assert_not_reached();
    }
    if (ispi->rtc_trace_events++ < 1024) {
        qemu_log_mask(LOG_UNIMP,
                      "miband PMU RTC read selector=31%02x value=%04x"
                      " virtual_ms=%" PRId64
                      " calendar=%04d-%02d-%02dT%02d:%02d:%02d rtc_clock=%s\n",
                      reg, data, virtual_ns / SCALE_MS,
                      calendar.tm_year + 1900, calendar.tm_mon + 1,
                      calendar.tm_mday, calendar.tm_hour,
                      calendar.tm_min, calendar.tm_sec, ispi_rtc_clock_name());
    }
    return data;
}

static void ispi_rtc_load(MiBandIspi *ispi)
{
    uint16_t *registers = ispi->regs[ISPI_PMU_CS][RTC_PAGE];
    unsigned second = rtc_from_bcd(registers[RTC_LOAD_TIME] & 0x7f);
    unsigned minute = rtc_from_bcd((registers[RTC_LOAD_TIME] >> 8) & 0x7f);
    unsigned hour = rtc_from_bcd(registers[RTC_LOAD_HOUR] & 0x3f);
    unsigned day = rtc_from_bcd(registers[RTC_LOAD_DATE] & 0x3f);
    unsigned month = rtc_from_bcd((registers[RTC_LOAD_DATE] >> 8) & 0x1f);
    unsigned year = rtc_from_bcd(registers[RTC_LOAD_YEAR] & 0xff);
    unsigned week = registers[RTC_LOAD_DATE] >> 13;
    struct tm calendar = { 0 };
    struct tm normalized;
    time_t seconds;

    registers[RTC_CONTROL] &= ~RTC_LOAD;
    if (second > 59 || minute > 59 || hour > 23 || day < 1 || day > 31 ||
        month < 1 || month > 12 || year > 99 || week < 1 || week > 7) {
        qemu_log_mask(LOG_UNIMP, "miband PMU RTC rejected invalid load\n");
        return;
    }
    calendar.tm_sec = second;
    calendar.tm_min = minute;
    calendar.tm_hour = hour;
    calendar.tm_mday = day;
    calendar.tm_mon = month - 1;
    calendar.tm_year = year + 100;
    seconds = mktimegm(&calendar);
    gmtime_r(&seconds, &normalized);
    if (normalized.tm_year != calendar.tm_year ||
        normalized.tm_mon != calendar.tm_mon ||
        normalized.tm_mday != calendar.tm_mday) {
        qemu_log_mask(LOG_UNIMP, "miband PMU RTC rejected invalid date\n");
        return;
    }
    ispi->rtc_base_seconds = seconds;
    ispi->rtc_start_ns = qemu_clock_get_ns(rtc_clock);
    ispi->rtc_base_week = week;
    ispi->regs[ISPI_PMU_CS][0][RTC_VALID_REG] |= RTC_VALID;
    qemu_log_mask(LOG_UNIMP,
                  "miband PMU RTC loaded calendar=%04d-%02d-%02dT%02d:%02d:%02d"
                  " virtual_ms=%" PRId64 " rtc_clock=%s\n",
                  calendar.tm_year + 1900, calendar.tm_mon + 1,
                  calendar.tm_mday, calendar.tm_hour,
                  calendar.tm_min, calendar.tm_sec,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / SCALE_MS,
                  ispi_rtc_clock_name());
}

static void ispi_trace(MiBandIspi *ispi, const char *kind, uint32_t value)
{
    /* Keep enough history to include the later GPADC/PMU transactions.  The
     * log is still bounded so a long-running guest cannot grow it forever. */
    if (ispi->trace_events++ < 4096) {
        unsigned reg = (value >> (16 + ISPI_CMD_SHIFT)) & 0xff;
        uint32_t pc = current_cpu ? CPU_GET_CLASS(current_cpu)->get_pc(current_cpu) : 0;

        qemu_log_mask(LOG_UNIMP,
                      "miband ISPI %s cs=%u ctrl=%08" PRIx32
                      " value=%08" PRIx32 " pc=%08" PRIx32
                      " page=%04x reg=%02x rx=%04x\n",
                      kind, ispi->chip_select, ispi->control, value, pc,
                      ispi->page[ispi->chip_select], reg,
                      (uint16_t)(ispi->rx_data >> ISPI_CMD_SHIFT));
    }
}

void miband_ispi_init(MiBandIspi *ispi)
{
    memset(ispi, 0, sizeof(*ispi));
    /* best1503 PMU chip ID is 0x153 << 4 on PMU chip-select 3. */
    ispi->regs[ISPI_PMU_CS][0][0] = 0x1530;
    /* Battery and thermistor values are raw ADC candidates.  They are only
     * exposed when gpadc-model is explicitly enabled on the machine. */
    for (unsigned i = 0; i < ARRAY_SIZE(ispi->gpadc_values); i++) {
        ispi->gpadc_values[i] = 0x6800;
    }
    ispi->gpadc_values[15] = 0x7c00;
    ispi->gpadc_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ispi_gpadc_tick, ispi);
}

void miband_ispi_set_gpadc_model(MiBandIspi *ispi, bool enabled)
{
    ispi->gpadc_model = enabled;
    ispi->gpadc_valid = 0;
    ispi_gpadc_schedule(ispi);
    ispi_pmu_update_irq(ispi);
    qemu_log_mask(LOG_UNIMP, "miband PMU GPADC model %s\n",
                  enabled ? "enabled (diagnostic)" : "disabled");
}

void miband_ispi_set_analog_mv(MiBandIspi *ispi, unsigned channel, unsigned mv)
{
    /* Ideal ADC transfer at the modeled default eFuse calibration points:
     * 15968 -> 800 mV and 20816 -> 1050 mV. Select the interval center. */
    assert(channel < ARRAY_SIZE(ispi->gpadc_values) && mv <= 1800);
    ispi->gpadc_values[channel] = (mv * 4848 + 113600 + 249) / 250 + 9;
}

void miband_ispi_set_pmu_status_model(MiBandIspi *ispi, bool enabled)
{
    ispi->pmu_status_model = enabled;

    /* sub_C0E7516 reads PMU selector 0x305e and returns
     * ((value ^ 4) >> 2) & 1.  A zero value therefore deterministically
     * enters the firmware's "pmu shutdown failed" noreturn path.  The bit
     * below is exposed only through this explicit diagnostic option. */
    if (enabled) {
        ispi->regs[ISPI_PMU_CS][0][0x5e] |= 0x0004;
    }
    qemu_log_mask(LOG_UNIMP, "miband PMU status model %s\n",
                  enabled ? "enabled (diagnostic)" : "disabled");
}

void miband_ispi_set_pmu_rtc_model(MiBandIspi *ispi, bool enabled)
{
    struct tm calendar;

    ispi->pmu_rtc_model = enabled;
    if (enabled) {
        qemu_get_timedate(&calendar, 0);
        ispi->rtc_base_seconds = mktimegm(&calendar);
        ispi->rtc_start_ns = qemu_clock_get_ns(rtc_clock);
        ispi->rtc_base_week = calendar.tm_wday + 1;
        ispi->regs[ISPI_PMU_CS][0][RTC_VALID_REG] |= RTC_VALID;
        qemu_log_mask(LOG_UNIMP,
                      "miband PMU RTC enabled seed=%04d-%02d-%02dT%02d:%02d:%02d"
                      " rtc_clock=%s\n",
                      calendar.tm_year + 1900, calendar.tm_mon + 1,
                      calendar.tm_mday, calendar.tm_hour,
                      calendar.tm_min, calendar.tm_sec, ispi_rtc_clock_name());
    }
}

bool miband_ispi_read(MiBandIspi *ispi, hwaddr address, unsigned size,
                      uint64_t *result)
{
    if (size != sizeof(uint32_t)) {
        return false;
    }

    if (address == ISPI_BASE + ISPI_DATA) {
        ispi->rx_ready = false;
        *result = ispi->rx_data;
        return true;
    }
    if (address == ISPI_BASE + ISPI_CONTROL) {
        *result = ispi->control;
        return true;
    }
    if (address == ISPI_BASE + ISPI_FIFO_STATUS) {
        *result = ISPI_TX_ROOM | (ispi->rx_ready ? ISPI_RX_READY : 0);
        return true;
    }
    return false;
}

bool miband_ispi_write(MiBandIspi *ispi, hwaddr address, unsigned size,
                       uint64_t value)
{
    unsigned reg;
    unsigned page;
    uint16_t data;

    if (size != sizeof(uint32_t)) {
        return false;
    }

    if (address == ISPI_BASE + ISPI_CONTROL) {
        ispi->control = value;
        ispi->chip_select = (value >> ISPI_CS_SHIFT) & ISPI_CS_MASK;
        ispi_trace(ispi, "control-write", value);
        return true;
    }
    if (address != ISPI_BASE + ISPI_DATA) {
        return false;
    }

    reg = (value >> (16 + ISPI_CMD_SHIFT)) & 0xff;
    page = ispi_page_index(ispi);
    if (value & ISPI_READ_CMD) {
        uint16_t data = ispi->regs[ispi->chip_select][page][reg];

        if (ispi->pmu_rtc_model && ispi->chip_select == ISPI_PMU_CS &&
            page == RTC_PAGE && reg >= RTC_READ_TIME && reg <= RTC_READ_YEAR) {
            data = ispi_rtc_read(ispi, reg);
        }
        if (ispi->gpadc_model && ispi->chip_select == ISPI_PMU_CS && page == 0) {
            if (reg == GPADC_REG_RAW_STATUS) {
                data = ispi->gpadc_valid;
            } else if (reg == GPADC_REG_STATUS_CLR) {
                data = ispi->gpadc_valid &
                       ispi->regs[ISPI_PMU_CS][0][GPADC_REG_IRQ_MASK];
            } else if (reg >= GPADC_REG_CH_DATA &&
                       reg < GPADC_REG_CH_DATA + ARRAY_SIZE(ispi->gpadc_values) &&
                       reg != RTC_VALID_REG) {
                data = ispi->gpadc_values[reg - GPADC_REG_CH_DATA];
            }
        }
        if (ispi->chip_select == ISPI_PMU_CS && page == 0 && reg == 0x5e) {
            if (ispi->charger && ispi->charger->enabled) {
                /* sub_C0E7516 (RAM 0x29b732) interprets selector 0x305e
                 * bit 2 as AC_ON. The external charger environment must
                 * override the older diagnostic's permanently inserted AC. */
                data = (ispi->regs[ISPI_PMU_CS][0][PMU_CHARGER_STATUS] &
                        ~PMU_CHARGER_AC_ON) |
                       (ispi->charger->charging ? PMU_CHARGER_AC_ON : 0);
            } else if (ispi->pmu_status_model) {
                data |= 0x0004;
            }
        }
        ispi->rx_data = (uint32_t)data << ISPI_CMD_SHIFT;
        ispi_trace(ispi, "read", value);
    } else {
        data = (value >> ISPI_CMD_SHIFT) & 0xffff;
        /* Offset zero is the page selector for the analog interface. It is
         * not the backing value returned by a subsequent PMU chip-id read. */
        if (reg == 0 && ispi_page_value(data)) {
            ispi->page[ispi->chip_select] = data;
        } else {
            if (ispi->charger && ispi->charger->enabled &&
                ispi->chip_select == ISPI_PMU_CS && page == 0 &&
                reg == PMU_CHARGER_STATUS) {
                ispi->regs[ISPI_PMU_CS][0][reg] &= ~(data & PMU_CHARGER_IRQS);
                ispi_pmu_update_irq(ispi);
            } else {
                ispi->regs[ispi->chip_select][page][reg] = data;
                if (ispi->chip_select == ISPI_PMU_CS && page == 0 &&
                    (reg == 0xf8 || reg == 0xfa || reg == 0xfe)) {
                    /* Analog PLL calibration: FA[15] starts calibration,
                     * F8[14,3] enables its loop, FE[5:4] opens calibration.
                     * FC[11] latches completion until the loop is disabled.
                     * reference exercises this hardware path before kernel startup. */
                    uint16_t *pmu = ispi->regs[ISPI_PMU_CS][0];
                    if ((pmu[0xf8] & 0x4008) == 0x4008 &&
                        (pmu[0xfa] & 0x8000) && (pmu[0xfe] & 0x30) == 0x30) {
                        pmu[0xfc] |= 0x0800;
                    } else if (!(pmu[0xf8] & 0x4000)) {
                        pmu[0xfc] &= ~0x0800;
                    }
                }
                if (ispi->chip_select == ISPI_PMU_CS && page == 0 &&
                    (reg == PMU_CHARGER_ENABLE || reg == PMU_UNIFIED_ENABLE ||
                     reg == GPADC_REG_IRQ_MASK)) {
                    ispi_pmu_update_irq(ispi);
                }
            }

            if (ispi->pmu_rtc_model && ispi->chip_select == ISPI_PMU_CS &&
                page == RTC_PAGE && reg == RTC_CONTROL && (data & RTC_LOAD)) {
                ispi_rtc_load(ispi);
            }

            if (ispi->gpadc_model && ispi->chip_select == ISPI_PMU_CS && page == 0) {
                if (reg == GPADC_REG_STATUS_CLR) {
                    ispi->gpadc_valid &= ~data;
                    ispi_pmu_update_irq(ispi);
                } else if (reg == GPADC_REG_START &&
                           (data & GPADC_START)) {
                    /* The AP enables the channel before asserting START. */
                    ispi_gpadc_sample(ispi);
                    if (!(ispi->regs[ISPI_PMU_CS][0][GPADC_REG_INTERVAL_ENABLE] &
                          GPADC_INTERVAL_MODE)) {
                        ispi->regs[ISPI_PMU_CS][0][GPADC_REG_START] &= ~GPADC_START;
                    }
                }
                if (reg == GPADC_REG_START || reg == GPADC_REG_CH_EN ||
                    reg == GPADC_REG_INTERVAL_ENABLE || reg == GPADC_REG_INTERVAL) {
                    ispi_gpadc_schedule(ispi);
                }
            }
        }
        ispi_trace(ispi, "write", value);
        ispi->rx_data = 0;
    }
    ispi->rx_ready = true;
    return true;
}
