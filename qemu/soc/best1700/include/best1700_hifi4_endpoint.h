/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BEST1700_HIFI4_ENDPOINT_H
#define BEST1700_HIFI4_ENDPOINT_H

#include "exec/memory.h"
#include "hw/irq.h"
#include "qapi/error.h"

typedef struct Best1700Hifi4Endpoint Best1700Hifi4Endpoint;

/* M55C0/DSPC0 channel zero; other CMU interrupt bits belong to their peers. */
#define BEST1700_HIFI4_IRQ_MASK 0x00000249u

Best1700Hifi4Endpoint *best1700_hifi4_new(AddressSpace *as,
                                        const char *profile_path,
                                        const char *audio_image,
                                        qemu_irq rx_irq, qemu_irq done_irq,
                                        Error **errp);
void best1700_hifi4_start(Best1700Hifi4Endpoint *s, uint32_t vector);
void best1700_hifi4_pause(Best1700Hifi4Endpoint *s, bool paused);
void best1700_hifi4_reset(Best1700Hifi4Endpoint *s);
void best1700_hifi4_free(Best1700Hifi4Endpoint *s);
uint32_t best1700_hifi4_pending(const Best1700Hifi4Endpoint *s);
void best1700_hifi4_set(Best1700Hifi4Endpoint *s, uint32_t bits);
void best1700_hifi4_clear(Best1700Hifi4Endpoint *s, uint32_t bits);
bool best1700_hifi4_service_ready(const Best1700Hifi4Endpoint *s);
char *best1700_hifi4_status_json(const Best1700Hifi4Endpoint *s);

#endif
