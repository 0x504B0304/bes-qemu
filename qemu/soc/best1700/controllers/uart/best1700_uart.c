#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "best1700_uart.h"

#define BEST1700_UART_DATA   0x00
#define BEST1700_UART_RSR    0x04
#define BEST1700_UART_STATUS 0x18
#define BEST1700_UART_CR     0x30
#define BEST1700_UART_LCR    0x2c
#define BEST1700_UART_IFLS   0x34
#define BEST1700_UART_IMSC   0x38
#define BEST1700_UART_RIS    0x3c
#define BEST1700_UART_MIS    0x40
#define BEST1700_UART_ICR    0x44
#define BEST1700_UART_DMACR  0x48
#define BEST1700_UART_TX_ON  0x101
#define BEST1700_UART_RX_ON  0x201
#define UART_INT_RX (1u << 4)
#define UART_INT_TX (1u << 5)
#define UART_INT_RT (1u << 6)
#define UART_INT_BE (1u << 9)
#define UART_INT_OE (1u << 10)
#define UART_DMA_RT_ENABLE (1u << 15)

static bool uart_dma_timeout_enabled(const Best1700Uart *uart)
{
    return (uart->regs[BEST1700_UART_LCR >> 2] & UART_DMA_RT_ENABLE) &&
           (uart->regs[BEST1700_UART_DMACR >> 2] & 1);
}

static bool uart_rx_enabled(const Best1700Uart *uart)
{
    return (uart->regs[BEST1700_UART_CR >> 2] & BEST1700_UART_RX_ON) ==
           BEST1700_UART_RX_ON;
}

static unsigned uart_fifo_depth(const Best1700Uart *uart)
{
    return (uart->regs[BEST1700_UART_LCR >> 2] & (1u << 4)) ?
           BEST1700_UART_FIFO_DEPTH : 1;
}

bool best1700_uart_dma_rx_pending(const Best1700Uart *uart)
{
    return uart_rx_enabled(uart) && uart->rx_count &&
           (uart->regs[BEST1700_UART_DMACR >> 2] & 1);
}

static void uart_notify_dma(Best1700Uart *uart)
{
    if (uart->dma_notify && best1700_uart_dma_rx_pending(uart)) {
        uart->dma_notify(uart->dma_opaque);
    }
}

static uint32_t uart_pending(const Best1700Uart *uart)
{
    static const unsigned thresholds[] = { 2, 4, 8, 12, 14 };
    unsigned selector = (uart->regs[BEST1700_UART_IFLS >> 2] >> 3) & 7;
    unsigned trigger = uart_fifo_depth(uart) == 1 ? 1 :
                       thresholds[MIN(selector, 4u)];
    uint32_t pending = uart->events;

    if (uart_rx_enabled(uart) && uart->rx_count >= trigger) {
        pending |= UART_INT_RX;
    }
    /* TX completes synchronously, so an enabled transmitter is below every
     * TX watermark. The guest disables its mask when its software queue drains. */
    if ((uart->regs[BEST1700_UART_CR >> 2] & BEST1700_UART_TX_ON) ==
        BEST1700_UART_TX_ON) {
        pending |= UART_INT_TX;
    }
    return pending;
}

static void uart_update_irq(Best1700Uart *uart)
{
    qemu_set_irq(uart->irq, (uart_pending(uart) &
                 uart->regs[BEST1700_UART_IMSC >> 2] & 0x7ff) != 0);
}

static void uart_arm_timeout(Best1700Uart *uart)
{
    uint64_t divisor = (uint64_t)(uart->regs[0x24 >> 2] & 0xffff) * 64 +
                       (uart->regs[0x28 >> 2] & 63);
    unsigned sampling = (uart->regs[0x4c >> 2] & 15) + 1;

    timer_del(uart->rx_timer);
    if (uart_rx_enabled(uart) &&
        (uart->rx_count || (uart_dma_timeout_enabled(uart) &&
                            uart->dma_rx_activity)) &&
        divisor && uart->clock_hz) {
        /* Receive timeout is 32 bit periods. Divisor has six fractional bits. */
        uint64_t ns = DIV_ROUND_UP(32ULL * sampling * divisor * NANOSECONDS_PER_SECOND,
                                  64ULL * uart->clock_hz);
        timer_mod(uart->rx_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ns);
    }
}

static void uart_timeout(void *opaque)
{
    Best1700Uart *uart = opaque;

    if (uart_rx_enabled(uart) &&
        (uart->rx_count || (uart_dma_timeout_enabled(uart) &&
                            uart->dma_rx_activity))) {
        uart->events |= UART_INT_RT;
        uart_update_irq(uart);
    }
}

static void uart_drain_pending(Best1700Uart *uart);

static void uart_push(Best1700Uart *uart, uint16_t byte)
{
    if (!uart_rx_enabled(uart)) {
        return;
    }
    if (uart->rx_count == uart_fifo_depth(uart)) {
        uart->errors |= 8;
        uart->events |= UART_INT_OE;
    } else {
        unsigned tail = (uart->rx_head + uart->rx_count) % BEST1700_UART_FIFO_DEPTH;
        uart->rx_fifo[tail] = byte;
        uart->rx_count++;
        if (byte & (1u << 10)) {
            uart->errors |= 4;
            uart->events |= UART_INT_BE;
        }
        uart->events &= ~UART_INT_RT;
        uart->dma_rx_activity = uart_dma_timeout_enabled(uart);
        uart_arm_timeout(uart);
    }
    uart_update_irq(uart);
    uart_notify_dma(uart);
}

static void uart_drain_pending(Best1700Uart *uart)
{
    while (uart->rx_pending_count && uart_rx_enabled(uart) &&
           uart->rx_count < uart_fifo_depth(uart)) {
        uint8_t byte = uart->rx_pending[uart->rx_pending_head];
        uart->rx_pending_head = (uart->rx_pending_head + 1) %
                                BEST1700_UART_PENDING_DEPTH;
        uart->rx_pending_count--;
        uart_push(uart, byte);
    }
}


static int uart_can_receive(void *opaque)
{
    Best1700Uart *uart = opaque;

    /* The host callback may deliver a whole line even when the guest UART
     * FIFO is disabled or currently full. Keep bounded pending input so
     * command and modem streams are drained in guest order instead of
     * dropping the tail of a burst. */
    return BEST1700_UART_PENDING_DEPTH - uart->rx_pending_count;
}

static void uart_receive(void *opaque, const uint8_t *buf, int size)
{
    Best1700Uart *uart = opaque;

    for (int i = 0; i < size; i++) {
        if (uart->rx_pending_count == BEST1700_UART_PENDING_DEPTH) {
            uart->errors |= 8;
            uart->events |= UART_INT_OE;
            break;
        }
        unsigned tail = (uart->rx_pending_head + uart->rx_pending_count) %
                        BEST1700_UART_PENDING_DEPTH;
        uart->rx_pending[tail] = buf[i];
        uart->rx_pending_count++;
    }
    uart_drain_pending(uart);
    qemu_chr_fe_accept_input(&uart->chr);
}

static void uart_event(void *opaque, QEMUChrEvent event)
{
    if (event == CHR_EVENT_BREAK) {
        uart_push(opaque, 1u << 10);
    }
}

static uint64_t best1700_uart_read(void *opaque, hwaddr address,
                                   unsigned size)
{
    Best1700Uart *uart = opaque;
    uint32_t value = 0;

    if (address < uart->size && !(address & 3) &&
        size <= sizeof(value)) {
        if (address == BEST1700_UART_DATA) {
            if (uart->rx_count) {
                value = uart->rx_fifo[uart->rx_head];
                uart->rx_head = (uart->rx_head + 1) % BEST1700_UART_FIFO_DEPTH;
                uart->rx_count--;
                uart_drain_pending(uart);
                /* DMA idle detection follows serial ingress even after DMA
                 * drains the FIFO. Data reads must not postpone that edge. */
                if (!uart_dma_timeout_enabled(uart)) {
                    uart->events &= ~UART_INT_RT;
                    uart_arm_timeout(uart);
                }
                uart_update_irq(uart);
                qemu_chr_fe_accept_input(&uart->chr);
            }
        } else if (address == BEST1700_UART_STATUS) {
            value = 0x81 | (!uart->rx_count ? 0x10 : 0) |
                    (uart->rx_count == uart_fifo_depth(uart) ? 0x40 : 0);
        } else if (address == BEST1700_UART_RSR) {
            value = uart->errors;
        } else if (address == BEST1700_UART_RIS) {
            value = uart_pending(uart);
        } else if (address == BEST1700_UART_MIS) {
            value = uart_pending(uart) & uart->regs[BEST1700_UART_IMSC >> 2];
        } else if (address == BEST1700_UART_ICR) {
            value = 0;
        } else {
            value = uart->regs[address >> 2];
        }
        uart->reads++;
        if (uart->reads <= 32) {
            qemu_log_mask(LOG_UNIMP,
                          "best1700 %s read offset=0x%03" HWADDR_PRIx
                          " value=0x%08" PRIx32 " pc=0x%08" PRIx32 "\n",
                          uart->name, address, value,
                          uart->pc ? *uart->pc : 0);
        }
    }
    if (size == 1) {
        return (value >> ((address & 3) * 8)) & 0xff;
    }
    if (size == 2) {
        return (value >> ((address & 2) * 8)) & 0xffff;
    }
    return value;
}

static void best1700_uart_write(void *opaque, hwaddr address,
                                uint64_t value, unsigned size)
{
    Best1700Uart *uart = opaque;
    uint32_t mask;
    unsigned shift;

    if (address >= uart->size || (address & 3) || size > sizeof(uint32_t)) {
        return;
    }
    if (address == BEST1700_UART_DATA) {
        if ((uart->regs[BEST1700_UART_CR >> 2] & BEST1700_UART_TX_ON) ==
            BEST1700_UART_TX_ON) {
            /* DR transmits only the low byte, including word/DMA writes.
             * Complete TX synchronously before reporting an empty FIFO. */
            uint8_t byte = value;

            if (uart->regs[BEST1700_UART_CR >> 2] & (1u << 7)) {
                uart_push(uart, byte);
            } else {
                qemu_chr_fe_write_all(&uart->chr, &byte, 1);
            }
        }
    } else if (address == BEST1700_UART_RSR) {
        uart->errors = 0;
    } else if (address == BEST1700_UART_ICR) {
        uart->events &= ~value;
        if (value & UART_INT_RT) {
            uart->dma_rx_activity = false;
            uart_arm_timeout(uart);
        }
    } else if (address != BEST1700_UART_STATUS &&
               address != BEST1700_UART_RIS &&
               address != BEST1700_UART_MIS) {
        shift = (address & 3) * 8;
        mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
        uint32_t previous = uart->regs[address >> 2];
        uart->regs[address >> 2] =
            (uart->regs[address >> 2] & ~(mask << shift)) |
            (((uint32_t)value & mask) << shift);
        if (address == BEST1700_UART_LCR &&
            ((previous ^ uart->regs[address >> 2]) & (1u << 4))) {
            uart->rx_count = uart->rx_head = 0;
            uart->dma_rx_activity = false;
            uart->events &= ~UART_INT_RT;
        }
        if (address == BEST1700_UART_CR || address == BEST1700_UART_LCR ||
            address == BEST1700_UART_DMACR || address == 0x24 ||
            address == 0x28 || address == 0x4c) {
            if (!uart_rx_enabled(uart)) {
                uart->events &= ~UART_INT_RT;
                uart->dma_rx_activity = false;
            }
            if (!uart_dma_timeout_enabled(uart)) {
                uart->dma_rx_activity = false;
            }
            uart_arm_timeout(uart);
            uart_drain_pending(uart);
            qemu_chr_fe_accept_input(&uart->chr);
        }
    }
    uart_update_irq(uart);
    if (address == BEST1700_UART_DMACR || address == BEST1700_UART_CR) {
        uart_notify_dma(uart);
    }
    uart->writes++;
    if (uart->writes <= 32) {
        qemu_log_mask(LOG_UNIMP,
                      "best1700 %s write offset=0x%03" HWADDR_PRIx
                      " value=0x%08" PRIx64 " pc=0x%08" PRIx32 "\n",
                      uart->name, address, value,
                      uart->pc ? *uart->pc : 0);
    }
}

static const MemoryRegionOps best1700_uart_ops = {
    .read = best1700_uart_read,
    .write = best1700_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

void best1700_uart_init(Best1700Uart *uart, Object *owner,
                        const char *name, unsigned size,
                        const uint32_t *pc)
{
    memset(uart, 0, sizeof(*uart));
    uart->size = size;
    uart->name = g_strdup(name);
    uart->pc = pc;
    uart->regs = g_new0(uint32_t, size / sizeof(uint32_t));
    uart->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, uart_timeout, uart);
    best1700_uart_reset(uart);
    memory_region_init_io(&uart->region, owner, &best1700_uart_ops, uart,
                          name, size);
}

void best1700_uart_add(Best1700Uart *uart, MemoryRegion *space,
                       hwaddr base, int priority)
{
    memory_region_add_subregion_overlap(space, base, &uart->region,
                                        priority);
}

void best1700_uart_reset(Best1700Uart *uart)
{
    /* Reset guest-visible state while keeping the connected host stream. */
    memset(uart->regs, 0, uart->size);
    uart->regs[0x4c >> 2] = 15; /* Default 16x oversampling. */
    timer_del(uart->rx_timer);
    uart->rx_count = uart->rx_head = 0;
    uart->rx_pending_head = uart->rx_pending_count = 0;
    uart->dma_rx_activity = false;
    uart->events = uart->errors = 0;
    uart->reads = 0;
    uart->writes = 0;
    uart_update_irq(uart);
}

void best1700_uart_set_pc(Best1700Uart *uart, const uint32_t *pc)
{
    uart->pc = pc;
}

void best1700_uart_set_chardev(Best1700Uart *uart, Chardev *chr)
{
    qemu_chr_fe_init(&uart->chr, chr, &error_fatal);
    qemu_chr_fe_set_handlers(&uart->chr, uart_can_receive, uart_receive,
                            uart_event, NULL, uart, NULL, true);
}

void best1700_uart_set_irq(Best1700Uart *uart, qemu_irq irq)
{
    uart->irq = irq;
    uart_update_irq(uart);
}

void best1700_uart_set_clock(Best1700Uart *uart, uint32_t frequency)
{
    uart->clock_hz = frequency;
    uart_arm_timeout(uart);
}

void best1700_uart_set_dma_notify(Best1700Uart *uart,
                                 void (*notify)(void *), void *opaque)
{
    uart->dma_notify = notify;
    uart->dma_opaque = opaque;
    uart_notify_dma(uart);
}
