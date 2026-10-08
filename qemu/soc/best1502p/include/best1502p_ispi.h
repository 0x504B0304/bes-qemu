#ifndef BEST1502P_ISPI_H
#define BEST1502P_ISPI_H

#include "exec/memory.h"
#include "qemu/timer.h"
#include "qemu/notify.h"

#define BEST1502P_ISPI_BASE 0x40009000u
#define BEST1502P_ISPI_SIZE 0x00000100u
#define BEST1502P_ISPI_CS_COUNT 16
#define BEST1502P_ISPI_PAGE_COUNT 6

/* The reference AP uses the BES ISPI register protocol for its on-die PMU.  The
 * register window is SoC IP; board-specific battery/charger behaviour is
 * intentionally left out of this first bring-up model. */
typedef struct Best1502pIspi {
    uint32_t regs[BEST1502P_ISPI_SIZE / sizeof(uint32_t)];
    uint16_t device_regs[BEST1502P_ISPI_CS_COUNT]
                       [BEST1502P_ISPI_PAGE_COUNT][256];
    uint16_t page[BEST1502P_ISPI_CS_COUNT];
    uint32_t control;
    uint8_t chip_select;
    uint32_t rx_data;
    bool rx_ready;
    unsigned trace_events;
    /* Board-provided raw analog inputs; UINT32_MAX means disconnected. */
    uint32_t adc_input[8];
    int64_t adc_deadline;
    uint16_t adc_pending;
    /* Battery-backed calendar domain survives AP reset. */
    int64_t rtc_base_seconds;
    int64_t rtc_start_ns;
    unsigned rtc_base_week;
    char *rtc_state_path;
    uint8_t rtc_image_digest[32];
    QEMUTimer *rtc_checkpoint;
    Notifier rtc_exit;
} Best1502pIspi;

void best1502p_ispi_init(Best1502pIspi *ispi);
/* Called only after the machine holds the flash journal's exclusive lock. */
void best1502p_ispi_open_rtc(Best1502pIspi *ispi, const char *flash_path,
                           const uint8_t digest[32]);
bool best1502p_ispi_read(Best1502pIspi *ispi, hwaddr offset,
                         unsigned size, uint64_t *result);
bool best1502p_ispi_write(Best1502pIspi *ispi, hwaddr offset,
                          unsigned size, uint64_t value);

#endif
