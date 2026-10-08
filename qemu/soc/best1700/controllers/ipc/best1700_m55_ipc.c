/*
 * BEST1700 M55C0/M55C1 hardware notification latches.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * These bits share the SYS CMU SET/CLR words with other IPC connections.
 * The register owner merges each connection's pending bits on reads and
 * distributes writes by mask. This model never accesses guest memory.
 */
#include "qemu/osdep.h"
#include "best1700_m55_ipc.h"

static void m55_ipc_update_irqs(Best1700M55Ipc *s)
{
    static const unsigned rx_shift[BEST1700_M55_IPC_SIDES] = { 21, 15 };
    static const unsigned done_shift[BEST1700_M55_IPC_SIDES] = { 18, 12 };

    for (unsigned side = 0; side < BEST1700_M55_IPC_SIDES; side++) {
        for (unsigned ch = 0; ch < BEST1700_M55_IPC_CHANNELS; ch++) {
            qemu_set_irq(s->rx[side][ch],
                         (s->pending >> (rx_shift[side] + ch)) & 1);
            qemu_set_irq(s->done[side][ch],
                         (s->pending >> (done_shift[side] + ch)) & 1);
        }
    }
}

void best1700_m55_ipc_init(Best1700M55Ipc *s)
{
    memset(s, 0, sizeof(*s));
}

void best1700_m55_ipc_connect_irq(Best1700M55Ipc *s, Best1700M55IpcSide side,
                                unsigned channel, qemu_irq rx, qemu_irq done)
{
    assert(side < BEST1700_M55_IPC_SIDES);
    assert(channel < BEST1700_M55_IPC_CHANNELS);
    s->rx[side][channel] = rx;
    s->done[side][channel] = done;
    m55_ipc_update_irqs(s);
}

uint32_t best1700_m55_ipc_pending(const Best1700M55Ipc *s)
{
    return s->pending;
}

void best1700_m55_ipc_set(Best1700M55Ipc *s, uint32_t bits)
{
    s->pending |= bits & BEST1700_M55_IPC_IRQ_MASK;
    m55_ipc_update_irqs(s);
}

void best1700_m55_ipc_clear(Best1700M55Ipc *s, uint32_t bits)
{
    /*
     * RX suspend clears IND. Receive completion is a separate firmware
     * write to DONE SET; acknowledging IND alone must not fabricate it.
     */
    s->pending &= ~(bits & BEST1700_M55_IPC_IRQ_MASK);
    m55_ipc_update_irqs(s);
}

void best1700_m55_ipc_reset(Best1700M55Ipc *s)
{
    s->pending = 0;
    m55_ipc_update_irqs(s);
}
