#include "qemu/osdep.h"
#include "qemu/log.h"
#include "best1700_apb_window.h"

static uint64_t best1700_apb_read(void *opaque, hwaddr address,
                                  unsigned size)
{
    Best1700ApbWindow *window = opaque;
    uint32_t value = 0;

    if (address < window->size && !(address & 3) && size <= sizeof(value)) {
        value = window->regs[address >> 2];
        window->reads++;
        if (window->reads <= 32) {
            qemu_log_mask(LOG_UNIMP,
                          "best1700 %s read offset=0x%04" HWADDR_PRIx
                          " value=0x%08" PRIx32 " pc=0x%08" PRIx32 "\n",
                          window->name, address, value,
                          window->pc ? *window->pc : 0);
        }
    }
    if (size == 1) {
        return value & 0xff;
    }
    if (size == 2) {
        return value & 0xffff;
    }
    return value;
}

static void best1700_apb_write(void *opaque, hwaddr address,
                               uint64_t value, unsigned size)
{
    Best1700ApbWindow *window = opaque;
    uint32_t *reg;
    uint32_t mask;
    unsigned shift;

    if (address >= window->size || (address & 3) || size > sizeof(uint32_t)) {
        return;
    }
    reg = &window->regs[address >> 2];
    shift = (address & 3) * 8;
    mask = size == 1 ? 0xffu : size == 2 ? 0xffffu : 0xffffffffu;
    *reg = (*reg & ~(mask << shift)) | ((uint32_t)value << shift);
    window->writes++;
    if (window->writes <= 32) {
        qemu_log_mask(LOG_UNIMP,
                      "best1700 %s write offset=0x%04" HWADDR_PRIx
                      " value=0x%08" PRIx64 " pc=0x%08" PRIx32 "\n",
                      window->name, address, value,
                      window->pc ? *window->pc : 0);
    }
}

static const MemoryRegionOps best1700_apb_ops = {
    .read = best1700_apb_read,
    .write = best1700_apb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

void best1700_apb_window_init(Best1700ApbWindow *window, Object *owner,
                              const char *name, unsigned size,
                              const uint32_t *pc)
{
    memset(window, 0, sizeof(*window));
    window->size = size;
    window->pc = pc;
    /* The caller commonly builds region names in a stack buffer.  Keep an
     * owned copy because the MemoryRegion callbacks outlive that buffer. */
    window->name = g_strdup(name);
    window->regs = g_new0(uint32_t, size / sizeof(uint32_t));
    memory_region_init_io(&window->region, owner, &best1700_apb_ops, window,
                          name, size);
}

void best1700_apb_window_add(Best1700ApbWindow *window, MemoryRegion *space,
                             hwaddr base, int priority)
{
    memory_region_add_subregion_overlap(space, base, &window->region,
                                        priority);
}

void best1700_apb_window_set_pc(Best1700ApbWindow *window,
                                const uint32_t *pc)
{
    window->pc = pc;
}

void best1700_apb_window_cleanup(Best1700ApbWindow *window)
{
    g_clear_pointer(&window->regs, g_free);
    g_clear_pointer(&window->name, g_free);
}
