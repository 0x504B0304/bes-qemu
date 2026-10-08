#ifndef BEST1700_APB_WINDOW_H
#define BEST1700_APB_WINDOW_H

#include "exec/memory.h"

/* Generic register-backed window used for BEST1700 subsystem APB blocks.
 * Individual IP models can replace registers with richer behaviour as the
 * firmware contract becomes known. */
typedef struct Best1700ApbWindow {
    MemoryRegion region;
    uint32_t *regs;
    unsigned size;
    uint32_t reads;
    uint32_t writes;
    char *name;
    const uint32_t *pc;
} Best1700ApbWindow;

void best1700_apb_window_init(Best1700ApbWindow *window, Object *owner,
                              const char *name, unsigned size,
                              const uint32_t *pc);
void best1700_apb_window_set_pc(Best1700ApbWindow *window,
                                const uint32_t *pc);
void best1700_apb_window_add(Best1700ApbWindow *window, MemoryRegion *space,
                             hwaddr base, int priority);
void best1700_apb_window_cleanup(Best1700ApbWindow *window);

#endif
