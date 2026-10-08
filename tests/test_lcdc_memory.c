/* Exercise LCDC DMA snapshots through the selected SoC memory resolver.
 * QEMU logging/time are inert here; the production LCDC and normalizer run. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "miband_lcdc.h"

int qemu_loglevel;
void qemu_log(const char *fmt, ...) { }
int64_t qemu_clock_get_ns(QEMUClockType type) { return 0; }
void warn_report(const char *fmt, ...) { abort(); }
int qemu_strtoui(const char *text, const char **end, int base,
                unsigned int *value)
{
    /* This test leaves the diagnostic limit unset. */
    abort();
}

typedef struct TestMemory {
    uint8_t bytes[12];
    uint32_t base;
    uint32_t address;
    size_t length;
    unsigned calls;
    bool reject;
} TestMemory;

static uint8_t *resolve(void *opaque, uint32_t address, size_t length)
{
    TestMemory *memory = opaque;

    memory->calls++;
    memory->address = address;
    memory->length = length;
    if (memory->reject || address < memory->base ||
        address - memory->base >= sizeof(memory->bytes) ||
        length > sizeof(memory->bytes) - (address - memory->base)) {
        return NULL;
    }
    return memory->bytes + address - memory->base;
}

static void capture(MiBandLcdc *lcdc, uint32_t address)
{
    lcdc->regs[0xf4 / 4] = address;
    lcdc->dma_frame_ready = false;
    memset(lcdc->dma_completed_frame, 0x7e, 16);
    miband_lcdc_note_dma_write(lcdc, address, 12);
}

int main(void)
{
    uint8_t ram[64], psram[64];
    MiBandLcdc lcdc;
    TestMemory memory = { .base = 0x70000000 };

    g_unsetenv("MIBAND_DISPLAY_DUMP_LIMIT");
    memset(ram, 0x11, sizeof(ram));
    memset(psram, 0x22, sizeof(psram));
    memset(memory.bytes, 0x33, sizeof(memory.bytes));
    miband_lcdc_init(&lcdc, (MiBandPanelEndpoint){0}, 32, 2, 2,
                     psram, sizeof(psram), ram, sizeof(ram));
    lcdc.regs[0x264 / 4] = 2 << 8; /* RGB888: four pixels occupy 12 bytes. */

    capture(&lcdc, 0x20000000);
    assert(lcdc.dma_frame_ready);
    assert(!memcmp(lcdc.dma_completed_frame, ram, 12));

    miband_lcdc_set_memory(&lcdc, &memory, resolve);
    capture(&lcdc, memory.base);
    assert(memory.calls == 1 && memory.address == memory.base);
    assert(memory.length == 12 && lcdc.dma_frame_ready);
    assert(!memcmp(lcdc.dma_completed_frame, memory.bytes, 12));

    /* An invalid resolver span must not complete or copy any bytes. */
    capture(&lcdc, memory.base + 1);
    assert(memory.calls == 2 && !lcdc.dma_frame_ready);
    for (unsigned i = 0; i < 16; i++) {
        assert(lcdc.dma_completed_frame[i] == 0x7e);
    }

    /* A callback can override an address accepted by the legacy aliases. */
    memory.base = 0x20000000;
    capture(&lcdc, memory.base);
    assert(memory.calls == 3 && lcdc.dma_frame_ready);
    assert(!memcmp(lcdc.dma_completed_frame, memory.bytes, 12));
    memory.reject = true;
    capture(&lcdc, memory.base);
    assert(memory.calls == 4 && !lcdc.dma_frame_ready);

    /* Removing it restores the original RAM/PSRAM alias behavior. */
    miband_lcdc_set_memory(&lcdc, NULL, NULL);
    capture(&lcdc, 0x00200000);
    assert(memory.calls == 4 && lcdc.dma_frame_ready);
    assert(!memcmp(lcdc.dma_completed_frame, ram, 12));
    capture(&lcdc, 0x38000000);
    assert(lcdc.dma_frame_ready);
    assert(!memcmp(lcdc.dma_completed_frame, psram, 12));

    g_free(lcdc.dma_completed_frame);
    g_free(lcdc.output_frame);
    puts("LCDC_MEMORY_OK resolver-snapshot override reject-span legacy-aliases");
    return 0;
}
