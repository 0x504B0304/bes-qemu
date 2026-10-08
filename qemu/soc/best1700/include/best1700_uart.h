#ifndef BEST1700_UART_H
#define BEST1700_UART_H

#include "exec/memory.h"
#include "chardev/char-fe.h"
#include "hw/irq.h"
#include "qemu/timer.h"

#define BEST1700_UART_FIFO_DEPTH 16
#define BEST1700_UART_PENDING_DEPTH 4096

typedef struct Best1700Uart {
    MemoryRegion region;
    CharBackend chr;
    uint32_t *regs;
    unsigned size;
    const char *name;
    const uint32_t *pc;
    uint32_t reads;
    uint32_t writes;
    qemu_irq irq;
    QEMUTimer *rx_timer;
    uint32_t clock_hz;
    uint32_t events;
    uint32_t errors;
    uint16_t rx_fifo[BEST1700_UART_FIFO_DEPTH];
    unsigned rx_head;
    unsigned rx_count;
    uint8_t rx_pending[BEST1700_UART_PENDING_DEPTH];
    unsigned rx_pending_head;
    unsigned rx_pending_count;
    bool dma_rx_activity;
    void (*dma_notify)(void *opaque);
    void *dma_opaque;
} Best1700Uart;

void best1700_uart_init(Best1700Uart *uart, Object *owner,
                        const char *name, unsigned size,
                        const uint32_t *pc);
void best1700_uart_reset(Best1700Uart *uart);
void best1700_uart_add(Best1700Uart *uart, MemoryRegion *space,
                       hwaddr base, int priority);
void best1700_uart_set_pc(Best1700Uart *uart, const uint32_t *pc);
void best1700_uart_set_chardev(Best1700Uart *uart, Chardev *chr);
void best1700_uart_set_irq(Best1700Uart *uart, qemu_irq irq);
void best1700_uart_set_clock(Best1700Uart *uart, uint32_t frequency);
bool best1700_uart_dma_rx_pending(const Best1700Uart *uart);
void best1700_uart_set_dma_notify(Best1700Uart *uart,
                                 void (*notify)(void *), void *opaque);

#endif
