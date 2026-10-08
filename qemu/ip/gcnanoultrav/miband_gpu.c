#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "miband_gpu.h"
#include "miband_gpu_worker.h"
#include "miband_gpu_pixels.h"
#include <math.h>

#define GPU_BASE 0x40220000
#define GPU_CLOCK_CONTROL 0x000
#define GPU_IDLE 0x004
#define GPU_IDLE_STATE 0x0b05
#define GPU_CHIP_ID 0x020
#define GPU_REVISION 0x024
#define GPU_CID 0x030
#define GPU_ECO_ID 0x0e8
#define GPU_INTR_STATUS 0x010
#define GPU_INTR_ENABLE 0x014
#define GPU_CMDBUF_ADDRESS 0x500
#define GPU_CMDBUF_SIZE 0x504
#define GPU_SOFT_RESET (1u << 12)
#define GPU_ISOLATE (1u << 19)
#define PSRAM_BASE 0x3c000000u
#define PSRAM_NC_BASE 0x38000000u
#define AP_RAM_BASE 0x20000000u
#define GPU_MAX_COMMAND_BYTES 0x20000u
#define GPU_MAX_CALL_DEPTH 4
#define GPU_MAX_EXECUTED_BYTES 0x80000u

/* Per-thread collector prevents the private replay from touching guest RAM or
 * legacy global diagnostic counters. Ordinary synchronous drawing marks its
 * exact writes dirty for the CPU/DMA/GPU page-version validator. */
typedef struct GpuPrivateRender {
    GArray *writes;
    GArray *inputs;
    bool invalid;
} GpuPrivateRender;

static GPrivate gpu_private_render = G_PRIVATE_INIT(NULL);

struct MiBandGpuAsync {
    CPUState *cpu;
    MiBandGpuWorker *worker;
    uint64_t generation;
    uint64_t fallback_unverified;
    uint64_t sync_count;
    uint64_t sync_total_ns;
    uint64_t sync_max_ns;
    uint64_t sync_samples[1024];
    bool requested;
};

static void gpu_collect_range(GArray *array, MiBandDisplayRange range,
                              GpuPrivateRender *render)
{
    if (array->len) {
        MiBandDisplayRange *last = &g_array_index(array, MiBandDisplayRange,
                                                 array->len - 1);
        if (last->block == range.block &&
            (uint64_t)last->offset + last->length == range.offset) {
            last->length += range.length;
            return;
        }
    }
    if (array->len >= 65536) {
        render->invalid = true;
        return;
    }
    g_array_append_val(array, range);
}

static void gpu_note_write(MiBandGpu *gpu, uint32_t address, size_t length)
{
    GpuPrivateRender *render = g_private_get(&gpu_private_render);
    MiBandDisplayRange range;
    if (!length) {
        return;
    }
    if (gpu->memory_ptr) {
        /* Custom buses execute synchronously; their resolver owns mapping. */
        return;
    }
    if (!render) {
        miband_display_memory_note_write(address, length);
    } else if (miband_display_normalize(address, length, gpu->ram_size,
                                       gpu->psram_size, &range)) {
        gpu_collect_range(render->writes, range, render);
    } else {
        render->invalid = true;
    }
}

/*
 * Experimental host-side evidence only.  The normal board never writes these
 * files; setting MIBAND_AGGRESSIVE_DUMP_DIR enables bounded command and
 * framebuffer snapshots for the independent aggressive run.
 */
static unsigned aggressive_dump_index;
static bool aggressive_dump_armed;
static bool aggressive_dump_released;

void miband_gpu_diag_touch_release(void)
{
    const char *arm_file = g_getenv("MIBAND_AGGRESSIVE_DUMP_ARM_FILE");
    const char *on_release = g_getenv("MIBAND_AGGRESSIVE_DUMP_ON_TOUCH_RELEASE");

    if (on_release && !strcmp(on_release, "1") && arm_file && *arm_file &&
        g_file_test(arm_file, G_FILE_TEST_EXISTS)) {
        if (!aggressive_dump_released) {
            qemu_log_mask(LOG_UNIMP,
                          "AGGRESSIVE_GPU_DUMP armed by touch release\n");
        }
        aggressive_dump_released = true;
    }
}

static bool gpu_aggressive_dump_enabled(void)
{
    const char *arm_file = g_getenv("MIBAND_AGGRESSIVE_DUMP_ARM_FILE");
    const char *on_release = g_getenv("MIBAND_AGGRESSIVE_DUMP_ON_TOUCH_RELEASE");

    if (on_release && !strcmp(on_release, "1")) {
        return aggressive_dump_released;
    }

    if (aggressive_dump_armed) {
        return true;
    }
    if (!arm_file || !*arm_file || g_file_test(arm_file, G_FILE_TEST_EXISTS)) {
        aggressive_dump_armed = true;
    }
    return aggressive_dump_armed;
}

static uint8_t *gpu_psram_ptr(MiBandGpu *gpu, uint32_t address,
                              size_t length);

static uint8_t *gpu_target_ptr(MiBandGpu *gpu, uint32_t address,
                               size_t length)
{
    MiBandDisplayRange range;
    if (gpu->memory_ptr) {
        return gpu->memory_ptr(gpu->memory_opaque, address, length, true);
    }
    if (!miband_display_normalize(address, length, gpu->ram_size,
                                  gpu->psram_size, &range)) {
        return NULL;
    }
    return (range.block == MIBAND_DISPLAY_AP_RAM ? gpu->ram : gpu->psram) +
           range.offset;
}

static void gpu_aggressive_dump_calls(MiBandGpu *gpu, const char *directory,
                                      const uint8_t *commands, size_t length,
                                      unsigned submit_index)
{
    size_t offset = 0;
    unsigned call_index = 0;

    while (offset + 8 <= length && call_index < 32) {
        uint32_t command = ldl_le_p(commands + offset);
        uint32_t opcode = command >> 28;
        size_t bytes = 8;

        if (opcode == 3) {
            unsigned count = (command >> 16) & 0xfff;
            bytes = ((size_t)count + 2) / 2 * 8;
        } else if (opcode == 4) {
            bytes = ((size_t)(command & 0x0fffffff) + 1) * 8;
        } else if (opcode == 6) {
            size_t called_length = (size_t)(command & 0x0fffffff) * 8;
            uint32_t address = ldl_le_p(commands + offset + 4);
            uint8_t *called = called_length <= GPU_MAX_COMMAND_BYTES ?
                              gpu_psram_ptr(gpu, address, called_length) : NULL;
            if (called) {
                char *path = g_strdup_printf(
                    "%s/submit-%03u-call-%03u-%08x.bin", directory,
                    submit_index, call_index, address);
                g_file_set_contents(path, (const char *)called,
                                    called_length, NULL);
                g_free(path);
            }
            call_index++;
        }
        if (!bytes || bytes > length - offset) {
            break;
        }
        offset += bytes;
    }
}

static void gpu_aggressive_dump(MiBandGpu *gpu, const uint8_t *commands,
                                size_t command_length, bool accepted,
                                unsigned paths_skipped)
{
    const char *directory = g_getenv("MIBAND_AGGRESSIVE_DUMP_DIR");
    const char *limit_text = g_getenv("MIBAND_AGGRESSIVE_DUMP_LIMIT");
    unsigned limit = 64;
    uint32_t target_size, width, height, stride, address;
    uint8_t *target;
    size_t target_length;
    char *path;
    char *meta;
    int meta_length;

    if (limit_text && *limit_text) {
        char *end;
        unsigned long parsed = strtoul(limit_text, &end, 10);

        if (!*end && parsed > 0 && parsed <= 4096) {
            limit = parsed;
        }
    }
    if (!directory || !*directory || aggressive_dump_index >= limit ||
        !gpu_aggressive_dump_enabled()) {
        return;
    }

    target_size = gpu->states[0xa13];
    width = target_size & 0xffffu;
    height = target_size >> 16;
    stride = gpu->states[0xa12];
    address = gpu->states[0xa11];
    target_length = (gpu->states[0xa10] == 0x109 ||
                     gpu->states[0xa10] == 0x10a) && stride % 4 == 0 ?
                    (size_t)(stride / 4 * 3) * height :
                    (size_t)stride * height;
    target = gpu_target_ptr(gpu, address, target_length);
    if (command_length > GPU_MAX_COMMAND_BYTES ||
        target_length > 16u * 1024u * 1024u ||
        (width && height && !target)) {
        target = NULL;
    }

    path = g_strdup_printf("%s/submit-%03u-command.bin", directory,
                           aggressive_dump_index);
    g_file_set_contents(path, (const char *)commands, command_length, NULL);
    g_free(path);
    gpu_aggressive_dump_calls(gpu, directory, commands, command_length,
                              aggressive_dump_index);
    if (target) {
        path = g_strdup_printf("%s/submit-%03u-target.bin", directory,
                               aggressive_dump_index);
        g_file_set_contents(path, (const char *)target, target_length, NULL);
        g_free(path);
    }
    path = g_strdup_printf("%s/submit-%03u.txt", directory,
                           aggressive_dump_index);
    meta = g_strdup_printf("cmd_address=0x%08x cmd_bytes=%zu target_address=0x%08x "
                           "format=%#x width=%u height=%u gpu_stride=%u "
                           "target_bytes=%zu rendered=%u paths=%u "
                           "acknowledged=%u accepted=%u paths_skipped=%u "
                           "rendering_complete=%u lcdc_source=%#x "
                           "lcdc_dimensions=%#x lcdc_bridge=%u\n",
                           gpu->cmd_address, command_length, address,
                           gpu->states[0xa10], width, height, stride,
                           target_length,
                           gpu->rendered, gpu->path_rendered,
                           gpu->acknowledged, accepted, paths_skipped,
                           accepted && !paths_skipped,
                           gpu->lcdc ? gpu->lcdc->regs[0xf4 / 4] : 0,
                           gpu->lcdc ? gpu->lcdc->regs[0xf0 / 4] : 0,
                           gpu->lcdc ? gpu->lcdc->bridge_active : 0);
    meta_length = (int)strlen(meta);
    g_file_set_contents(path, meta, meta_length, NULL);
    g_free(meta);
    g_free(path);
    qemu_log_mask(LOG_UNIMP,
                  "AGGRESSIVE_GPU_DUMP index=%u cmd=%#x bytes=%zu target=%#x "
                  "size=%ux%u stride=%u accepted=%u\n", aggressive_dump_index,
                  gpu->cmd_address, command_length, address, width, height,
                  stride, accepted);
    aggressive_dump_index++;
}

static uint8_t *gpu_psram_ptr(MiBandGpu *gpu, uint32_t address, size_t length)
{
    MiBandDisplayRange range;
    GpuPrivateRender *render = g_private_get(&gpu_private_render);
    if (gpu->memory_ptr) {
        return gpu->memory_ptr(gpu->memory_opaque, address, length, false);
    }
    if (!miband_display_normalize(address, length, gpu->ram_size,
                                  gpu->psram_size, &range) ||
        range.block != MIBAND_DISPLAY_PSRAM) {
        return NULL;
    }
    if (render) {
        gpu_collect_range(render->inputs, range, render);
    }
    return gpu->psram + range.offset;
}

/* Capture the gradient at the draw, before its source can be reused.
 * This only runs when the existing aggressive dump is enabled. */
static void gpu_dump_gradient_source(MiBandGpu *gpu, const uint8_t *path_data,
                                      size_t path_length)
{
    static bool attempted;
    const char *root;
    uint32_t source_width, source_height, source_stride;
    uint32_t target_width, target_height, target_stride;
    uint64_t source_bytes, target_bytes;
    const uint8_t *source, *target;
    g_autofree char *directory = NULL;
    g_autofree uint32_t *states_le = NULL;
    GString *metadata;
    char *filename;
    bool ok;
    unsigned i;

    if (g_private_get(&gpu_private_render) ||
        attempted || gpu->states[0xa25] != 0x01011107) {
        return;
    }
    root = g_getenv("MIBAND_AGGRESSIVE_DUMP_DIR");
    if (!root || !*root || !gpu_aggressive_dump_enabled()) {
        return;
    }
    attempted = true;
    source_width = gpu->states[0xa2f] & 0xffff;
    source_height = gpu->states[0xa2f] >> 16;
    source_stride = gpu->states[0xa2b];
    source_bytes = (uint64_t)source_stride * source_height;
    source = source_bytes && source_bytes <= 16u * 1024u * 1024u ?
             gpu_psram_ptr(gpu, gpu->states[0xa29], source_bytes) : NULL;
    if (!source || !source_width || !source_height ||
        !path_data || !path_length || path_length > GPU_MAX_COMMAND_BYTES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "miband GPU gradient source dump unavailable "
                      "source=%08x bytes=%" PRIu64 " path_bytes=%zu\n",
                      gpu->states[0xa29], source_bytes, path_length);
        return;
    }
    directory = g_build_filename(root, "gradient-source", NULL);
    if (g_mkdir_with_parents(directory, 0755) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "miband GPU gradient source dump mkdir failed: %s\n",
                      directory);
        return;
    }
    filename = g_build_filename(directory, "source.bin", NULL);
    ok = g_file_set_contents(filename, (const char *)source, source_bytes, NULL);
    g_free(filename);
    states_le = g_new(uint32_t, ARRAY_SIZE(gpu->states));
    for (i = 0; i < ARRAY_SIZE(gpu->states); i++) {
        states_le[i] = cpu_to_le32(gpu->states[i]);
    }
    filename = g_build_filename(directory, "states-le.bin", NULL);
    ok = g_file_set_contents(filename, (const char *)states_le,
                             sizeof(gpu->states), NULL) && ok;
    g_free(filename);
    filename = g_build_filename(directory, "path.bin", NULL);
    ok = g_file_set_contents(filename, (const char *)path_data,
                             path_length, NULL) && ok;
    g_free(filename);

    target_width = gpu->states[0xa13] & 0xffff;
    target_height = gpu->states[0xa13] >> 16;
    target_stride = gpu->states[0xa12];
    if (gpu->states[0xa10] == 0x109 || gpu->states[0xa10] == 0x10a) {
        target_stride = target_stride / 4 * 3;
    }
    target_bytes = (uint64_t)target_stride * target_height;
    target = target_bytes && target_bytes <= 16u * 1024u * 1024u ?
             gpu_target_ptr(gpu, gpu->states[0xa11], target_bytes) : NULL;
    if (target) {
        filename = g_build_filename(directory, "target-before.bin", NULL);
        ok = g_file_set_contents(filename, (const char *)target,
                                 target_bytes, NULL) && ok;
        g_free(filename);
    }
    metadata = g_string_new(NULL);
    g_string_append_printf(metadata,
        "diagnostic_only=1 source_format=%#x source_address=%#x "
        "source_width=%u source_height=%u source_stride=%u "
        "source_bytes=%" PRIu64 " path_bytes=%zu states_endian=little "
        "states_words=%zu cmd_address=%#x cmd_bytes=%u submit_dump_index=%u "
        "target_format=%#x target_address=%#x target_width=%u target_height=%u "
        "target_memory_stride=%u target_bytes=%" PRIu64 " target_saved=%u "
        "virtual_ns=%" PRIi64 "\n",
        gpu->states[0xa25], gpu->states[0xa29], source_width, source_height,
        source_stride, source_bytes, path_length, ARRAY_SIZE(gpu->states),
        gpu->cmd_address, gpu->cmd_size * 8, aggressive_dump_index,
        gpu->states[0xa10], gpu->states[0xa11], target_width, target_height,
        target_stride, target_bytes, target != NULL,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    for (i = 0xa00; i <= 0xa3a; i++) {
        g_string_append_printf(metadata, "state_%03x=%08x\n", i,
                               gpu->states[i]);
    }
    filename = g_build_filename(directory, "metadata.txt", NULL);
    ok = g_file_set_contents(filename, metadata->str, metadata->len, NULL) && ok;
    g_free(filename);
    g_string_free(metadata, true);
    qemu_log_mask(LOG_GUEST_ERROR,
                  "miband GPU gradient source dump saved=%u dir=%s "
                  "source=%08x bytes=%" PRIu64 " cmd=%08x submit=%u\n",
                  ok, directory, gpu->states[0xa29], source_bytes,
                  gpu->cmd_address, aggressive_dump_index);
}

static float gpu_float(uint32_t bits)
{
    float result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

/* Opt-in, bounded DRAW inputs for firmware-state regression.  Snapshot at
 * consumption time: submission-end memory can already contain later writes. */
static void gpu_dump_draw_inputs(MiBandGpu *gpu, const uint8_t *data,
                                  size_t length, bool rectangle)
{
    static GHashTable *seen;
    static unsigned saved;
    static uint64_t draw_id;
    const char *limit_text = g_getenv("MIBAND_GPU_DRAW_DUMP_LIMIT");
    const char *root = g_getenv("MIBAND_AGGRESSIVE_DUMP_DIR");
    unsigned long limit;
    char *end;
    g_autofree char *key = NULL;
    g_autofree char *directory = NULL;
    g_autofree uint32_t *states_le = NULL;
    GString *meta;
    uint32_t mode = gpu->states[0xa00];
    uint32_t source_format = gpu->states[0xa25];
    uint32_t sw = gpu->states[0xa2f] & 0xffff;
    uint32_t sh = gpu->states[0xa2f] >> 16;
    uint32_t ss = gpu->states[0xa2b];
    uint32_t tw = gpu->states[0xa13] & 0xffff;
    uint32_t th = gpu->states[0xa13] >> 16;
    uint32_t ts = gpu->states[0xa12];
    bool textured = (mode & 0x7000) != 0;
    size_t source_bytes, target_bytes;
    const uint8_t *source, *target;
    unsigned i;
    char *path;
    bool ok;

    /* Private replay must not write files, sample the QEMU virtual clock,
     * or mutate shared diagnostic counters. */
    if (g_private_get(&gpu_private_render)) {
        return;
    }
    draw_id++;
    if (!root || !*root || !limit_text || !*limit_text ||
        !gpu_aggressive_dump_enabled()) {
        return;
    }
    limit = strtoul(limit_text, &end, 10);
    if (*end || !limit || limit > 4096 || saved >= limit) {
        return;
    }
    /* Color/alpha modifiers and off-axis transforms distinguish requests;
     * dimensions/addresses intentionally do not consume one slot per glyph. */
    key = g_strdup_printf("%u:%08x:%08x:%08x:%08x:%08x:%08x:%08x:%u:%u:%u:%u",
        rectangle, mode, gpu->states[0xa10], textured ? source_format : 0,
        gpu->states[0xa34], gpu->states[0xad1],
        (mode & 0x7000) == 0x2000 ? gpu->states[0xa02] :
        textured ? 0 : gpu->states[0xa02] & 0xff000000,
        textured ? gpu->states[0xa27] : 0, sh == 1,
        (gpu->states[0xa1d] & 0x7fffffff) != 0 ||
        (gpu->states[0xa20] & 0x7fffffff) != 0,
        gpu->states[0xa11] >= AP_RAM_BASE &&
        gpu->states[0xa11] - AP_RAM_BASE < gpu->ram_size,
        gpu->states[0xa1a] != 0x3f800000 ||
        (gpu->states[0xa1e] & 0x7fffffff) != 0 ||
        (gpu->states[0xa22] & 0x7fffffff) != 0);
    if (!seen) {
        seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    if (g_hash_table_contains(seen, key)) {
        return;
    }
    g_hash_table_add(seen, g_strdup(key));
    if ((source_format == 0x21000100 || source_format == 0x21020100 ||
         source_format == 0x21030100 ||
         source_format == 0x41000100 || source_format == 0x41020100) &&
        ss % 4 == 0) {
        ss = ss / 4 * 3;
    }
    if ((gpu->states[0xa10] == 0x109 ||
         gpu->states[0xa10] == 0x10a) && ts % 4 == 0) {
        ts = ts / 4 * 3;
    }
    source_bytes = (size_t)ss * sh;
    target_bytes = (size_t)ts * th;
    source = textured && source_bytes && source_bytes <= 16u * 1024u * 1024u ?
             gpu_psram_ptr(gpu, gpu->states[0xa29], source_bytes) : NULL;
    target = target_bytes && target_bytes <= 16u * 1024u * 1024u ?
             gpu_target_ptr(gpu, gpu->states[0xa11], target_bytes) : NULL;
    directory = g_strdup_printf("%s/draw-inputs/draw-%03u", root, saved);
    if (g_mkdir_with_parents(directory, 0755) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "GPU DRAW input mkdir failed: %s\n",
                      directory);
        return;
    }
    states_le = g_new(uint32_t, ARRAY_SIZE(gpu->states));
    for (i = 0; i < ARRAY_SIZE(gpu->states); i++) {
        stl_le_p(&states_le[i], gpu->states[i]);
    }
    path = g_build_filename(directory, "states-le.bin", NULL);
    ok = g_file_set_contents(path, (const char *)states_le,
                             sizeof(gpu->states), NULL);
    g_free(path);
    path = g_build_filename(directory, "draw.bin", NULL);
    ok &= g_file_set_contents(path, (const char *)data, length, NULL);
    g_free(path);
    if (source) {
        path = g_build_filename(directory, "source.bin", NULL);
        ok &= g_file_set_contents(path, (const char *)source, source_bytes, NULL);
        g_free(path);
    }
    if (target) {
        path = g_build_filename(directory, "target-before.bin", NULL);
        ok &= g_file_set_contents(path, (const char *)target, target_bytes, NULL);
        g_free(path);
    }
    meta = g_string_new(NULL);
    g_string_append_printf(meta,
        "layer=GPU-draw-input draw_id=%" PRIu64 " capture_index=%u "
        "submit_dump_index=%u rectangle=%u mode=%#x source_format=%#x "
        "source_address=%#x source_width=%u source_height=%u "
        "source_memory_stride=%u source_saved=%u source_bytes=%zu "
        "target_format=%#x target_address=%#x target_width=%u "
        "target_height=%u target_memory_stride=%u target_saved=%u "
        "target_bytes=%zu draw_bytes=%zu virtual_ns=%" PRId64 " saved=%u\n",
        draw_id, saved, aggressive_dump_index, rectangle, mode, source_format,
        gpu->states[0xa29], sw, sh, ss, source != NULL, source_bytes,
        gpu->states[0xa10], gpu->states[0xa11], tw, th, ts,
        target != NULL, target_bytes, length,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), ok);
    path = g_build_filename(directory, "metadata.txt", NULL);
    g_file_set_contents(path, meta->str, meta->len, NULL);
    qemu_log_mask(LOG_UNIMP, "GPU DRAW input snapshot %s %s", directory, meta->str);
    g_free(path);
    g_string_free(meta, true);
    saved++;
}

typedef enum GpuSourceKind {
    GPU_SOURCE_A8_LINEAR,
    GPU_SOURCE_INDEX8_POINT,
    GPU_SOURCE_INDEX8_BILINEAR,
    GPU_SOURCE_BGRA8888_POINT,
    GPU_SOURCE_BGRA8888_BILINEAR,
    GPU_SOURCE_BGRA8888_LINEAR_PAD,
    GPU_SOURCE_BGR888_POINT,
    GPU_SOURCE_BGR888_BILINEAR,
    GPU_SOURCE_BGR888_GAUSSIAN,
    GPU_SOURCE_BGR565_POINT,
    GPU_SOURCE_BGR565_BILINEAR,
    GPU_SOURCE_BGRA5551_POINT,
    GPU_SOURCE_BGRA5658_POINT,
    GPU_SOURCE_BGRA5658_BILINEAR,
} GpuSourceKind;

static bool gpu_source_layout(MiBandGpu *gpu, uint32_t format,
                              uint32_t register_stride, uint32_t width,
                              uint32_t height, uint32_t *memory_stride,
                              GpuSourceKind *kind)
{
    unsigned bytes;

    /* All captured COLOR borders are transparent. Other border colours have
     * not been verified, so do not silently accept an untested combination. */
    if (gpu->states[0xa27] != 0) {
        return false;
    }
    *memory_stride = register_stride;
    switch (format) {
    case 0x01010102:
        /* VGLite A8 (2), LINEAR (0x10000), source premultiply.
         * reference uses this for MULTIPLY-coloured glyph rectangles. */
        *kind = GPU_SOURCE_A8_LINEAR;
        bytes = 1;
        break;
    case 0x01000900:
        *kind = GPU_SOURCE_INDEX8_POINT;
        bytes = 1;
        break;
    case 0x01020900:
        *kind = GPU_SOURCE_INDEX8_BILINEAR;
        bytes = 1;
        break;
    case 0x01000107:
        *kind = GPU_SOURCE_BGRA8888_POINT;
        bytes = 4;
        break;
    case 0x01020107:
        *kind = GPU_SOURCE_BGRA8888_BILINEAR;
        bytes = 4;
        break;
    case 0x01011107:
        /* The captured LINEAR paint is a single opaque BGRA ramp with PAD.
         * Multi-row and translucent ramps have no draw-time evidence yet. */
        if (height != 1) {
            return false;
        }
        *kind = GPU_SOURCE_BGRA8888_LINEAR_PAD;
        bytes = 4;
        break;
    case 0x01000105:
    case 0x01020105:
        *kind = format == 0x01000105 ? GPU_SOURCE_BGR565_POINT :
                                     GPU_SOURCE_BGR565_BILINEAR;
        bytes = 2;
        break;
    case 0x01000104:
        *kind = GPU_SOURCE_BGRA5551_POINT;
        bytes = 2;
        break;
    case 0x41000100:
    case 0x41020100:
        if (register_stride % 4) {
            return false;
        }
        *kind = format == 0x41000100 ? GPU_SOURCE_BGRA5658_POINT :
                                     GPU_SOURCE_BGRA5658_BILINEAR;
        *memory_stride = register_stride / 4 * 3;
        bytes = 3;
        break;
    case 0x21000100:
    case 0x21020100:
    case 0x21030100:
        /* Firmware produces packed BGR24 with target a10=0x109, then uses
         * POINT for page motion and BI_LINEAR+MULTIPLY for the guide fade.
         * Its register strides are in 4-byte units for both operations. */
        if (register_stride % 4) {
            return false;
        }
        *kind = format == 0x21000100 ? GPU_SOURCE_BGR888_POINT :
                format == 0x21020100 ? GPU_SOURCE_BGR888_BILINEAR :
                                      GPU_SOURCE_BGR888_GAUSSIAN;
        *memory_stride = register_stride / 4 * 3;
        bytes = 3;
        break;
    default:
        return false;
    }
    return (uint64_t)*memory_stride >= (uint64_t)width * bytes;
}

static bool gpu_source_content_supported(MiBandGpu *gpu, const uint8_t *source,
                                          uint32_t width, GpuSourceKind kind)
{
    static const unsigned affine_states[] = {
        0xa18, 0xa19, 0xa1c, 0xa1d, 0xa20, 0xa21,
    };
    unsigned i;

    if (gpu_float(gpu->states[0xa1a]) != 1.0f ||
        gpu_float(gpu->states[0xa1e]) != 0.0f ||
        gpu_float(gpu->states[0xa22]) != 0.0f) {
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(affine_states); i++) {
        if (!isfinite(gpu_float(gpu->states[affine_states[i]]))) {
            return false;
        }
    }
    if (kind == GPU_SOURCE_BGRA8888_LINEAR_PAD) {
        for (i = 0; i < width; i++) {
            if (source[(size_t)i * 4 + 3] != 255) {
                return false;
            }
        }
    }
    if (kind == GPU_SOURCE_BGR888_GAUSSIAN) {
        uint64_t centre = gpu->states[0xad3];
        uint64_t axial = gpu->states[0xad5];
        uint64_t corner = gpu->states[0xad7];
        uint64_t sum = centre + 4 * axial + 4 * corner;

        if (centre > 256 || axial > 256 || corner > 256 || !sum || sum > 256) {
            return false;
        }
    }
    return true;
}

static uint32_t gpu_source_sample(MiBandGpu *gpu, const uint8_t *src,
                                  uint32_t stride, int x, int y,
                                  GpuSourceKind kind)
{
    if (kind == GPU_SOURCE_BGRA5551_POINT) {
        return miband_gpu_pixel_read(src + (size_t)y * stride + (size_t)x * 2,
                                    0x105);
    }
    if (kind == GPU_SOURCE_A8_LINEAR) {
        unsigned alpha = src[(size_t)y * stride + x];
        return alpha * 0x01010101u;
    }
    if (kind == GPU_SOURCE_BGRA8888_POINT ||
        kind == GPU_SOURCE_BGRA8888_BILINEAR ||
        kind == GPU_SOURCE_BGRA8888_LINEAR_PAD) {
        return ldl_le_p(src + (size_t)y * stride + (size_t)x * 4);
    }
    if (kind == GPU_SOURCE_BGR888_POINT ||
        kind == GPU_SOURCE_BGR888_BILINEAR ||
        kind == GPU_SOURCE_BGR888_GAUSSIAN) {
        const uint8_t *pixel = src + (size_t)y * stride + (size_t)x * 3;

        return 0xff000000u | pixel[0] | ((uint32_t)pixel[1] << 8) |
               ((uint32_t)pixel[2] << 16);
    }
    if (kind == GPU_SOURCE_BGR565_POINT ||
        kind == GPU_SOURCE_BGR565_BILINEAR) {
        return miband_gpu_pixel_read(src + (size_t)y * stride + (size_t)x * 2,
                                      0x101);
    }
    if (kind == GPU_SOURCE_BGRA5658_POINT ||
        kind == GPU_SOURCE_BGRA5658_BILINEAR) {
        return miband_gpu_pixel_read(src + (size_t)y * stride + (size_t)x * 3,
                                      0x10a);
    }
    return gpu->states[0xb00 + src[(size_t)y * stride + x]];
}

static uint32_t gpu_source_color_tap(MiBandGpu *gpu, const uint8_t *src,
                                     uint32_t stride, uint32_t width,
                                     uint32_t height, int x, int y,
                                     GpuSourceKind kind)
{
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return gpu->states[0xa27];
    }
    return gpu_source_sample(gpu, src, stride, x, y, kind);
}

static uint32_t gpu_sample_source(MiBandGpu *gpu, const uint8_t *src,
                                  uint32_t stride, uint32_t width,
                                  uint32_t height, float u, float v,
                                  GpuSourceKind kind)
{
    bool pad = kind == GPU_SOURCE_BGRA8888_LINEAR_PAD;
    bool point = kind == GPU_SOURCE_INDEX8_POINT ||
                 kind == GPU_SOURCE_BGRA8888_POINT ||
                 kind == GPU_SOURCE_BGR888_POINT ||
                 kind == GPU_SOURCE_BGR565_POINT ||
                 kind == GPU_SOURCE_BGRA5551_POINT ||
                 kind == GPU_SOURCE_BGRA5658_POINT;
    int x0, y0, x1, y1, channel;
    float fx, fy;
    uint32_t color[4], result = 0;

    if (!isfinite(u) || !isfinite(v)) {
        return gpu->states[0xa27];
    }
    if (kind == GPU_SOURCE_A8_LINEAR) {
        /* SDK LINEAR shifts only the horizontal coordinate by half a
         * texel. Rows retain POINT sampling; COLOR supplies border taps. */
        if (u <= -1 || u >= width || v < 0 || v >= height) {
            return gpu->states[0xa27];
        }
        x0 = floorf(u);
        fx = u - x0;
        unsigned a0 = gpu_source_color_tap(gpu, src, stride, width,
                                           height, x0, (int)v, kind) >> 24;
        unsigned a1 = gpu_source_color_tap(gpu, src, stride, width,
                                           height, x0 + 1, (int)v, kind) >> 24;
        unsigned alpha = a0 * (1 - fx) + a1 * fx + 0.5f;
        return alpha * 0x01010101u;
    }
    if (kind == GPU_SOURCE_BGR888_GAUSSIAN) {
        unsigned sums[4] = {0};

        /* AD3 is the centre, AD5 the four axial neighbours and AD7 the
         * four corners. Coefficients are unsigned Q0.8; retain the guest's
         * quantization instead of renormalizing a truncated kernel. COLOR
         * borders apply independently to each tap. Rounding and the centre
         * convention are model policy pending a physical pixel capture. */
        if (u <= -2 || v <= -2 || u >= width + 1 || v >= height + 1) {
            return gpu->states[0xa27];
        }
        x0 = floorf(u);
        y0 = floorf(v);
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                unsigned weight = gpu->states[!dx && !dy ? 0xad3 :
                                               dx && dy ? 0xad7 : 0xad5];
                uint32_t tap = gpu_source_color_tap(gpu, src, stride,
                    width, height, x0 + dx, y0 + dy, kind);
                for (channel = 0; channel < 4; channel++) {
                    sums[channel] += ((tap >> (channel * 8)) & 255) * weight;
                }
            }
        }
        for (channel = 0; channel < 4; channel++) {
            result |= MIN((sums[channel] + 128) >> 8, 255u) << (channel * 8);
        }
        return result;
    }
    if (point) {
        /* POINT coordinates already include the firmware's pixel-centre
         * offset. Truncate in-range coordinates; COLOR applies outside. */
        if (u < 0 || v < 0 || u >= width || v >= height) {
            return gpu->states[0xa27];
        }
        return gpu_source_sample(gpu, src, stride, (int)u, (int)v, kind);
    }
    if (pad) {
        /* LINEAR paint uses its programmed u directly, with no extra -.5.
         * The captured source is one row, so every finite v maps to row 0. */
        u = CLAMP(u, 0, width - 1);
        v = 0;
    } else if (u <= -1 || v <= -1 || u >= width || v >= height) {
        return gpu->states[0xa27];
    }
    x0 = floorf(u);
    y0 = floorf(v);
    x1 = pad ? MIN(x0 + 1, (int)width - 1) : x0 + 1;
    y1 = pad ? 0 : y0 + 1;
    fx = u - x0;
    fy = v - y0;
    /* BI_LINEAR uses four taps of premultiplied source colours. COLOR is
     * evaluated per tap, rather than extending an edge texel with PAD. */
    color[0] = gpu_source_color_tap(gpu, src, stride, width, height,
                                    x0, y0, kind);
    color[1] = gpu_source_color_tap(gpu, src, stride, width, height,
                                    x1, y0, kind);
    color[2] = gpu_source_color_tap(gpu, src, stride, width, height,
                                    x0, y1, kind);
    color[3] = gpu_source_color_tap(gpu, src, stride, width, height,
                                    x1, y1, kind);
    for (channel = 0; channel < 4; channel++) {
        unsigned shift = channel * 8;
        float top = ((color[0] >> shift) & 0xff) * (1 - fx) +
                    ((color[1] >> shift) & 0xff) * fx;
        float bottom = ((color[2] >> shift) & 0xff) * (1 - fx) +
                       ((color[3] >> shift) & 0xff) * fx;
        result |= (uint32_t)(top * (1 - fy) + bottom * fy + 0.5f) << shift;
    }
    return result;
}

/* The captured firmware requests SRC_OVER and solid DST_IN. Texture image
 * MULTIPLY is separate from blend MULTIPLY: it modulates RGBA by a02 first.
 * In particular 10002102 must not fall through to a solid-colour path. */
static bool gpu_paint_state_supported(MiBandGpu *gpu, bool rectangle)
{
    uint32_t mode = gpu->states[0xa00];

    if (gpu->states[0xad1] != 0) {
        return false;
    }
    if (rectangle) {
        /* Bit 7 selects the command FIFO workaround, not pixel semantics.
         * NORMAL/MULTIPLY textures use the same pixel blend operations
         * as paths; NONE is used for offscreen filter intermediates. */
        return mode == 0x10000102 || (mode & ~0x80u) == 0x10001101 ||
               (mode & ~0x80u) == 0x10001001 ||
               (mode & ~0x80u) == 0x10002101 ||
               (mode == 0x00002181 && gpu->states[0xa25] == 0x01010102);
    }
    return mode == 0x10000102 || mode == 0x10000402 ||
           mode == 0x10001102 || mode == 0x10002102;
}

static unsigned gpu_paint_pixel(MiBandGpu *gpu, uint8_t *pixel,
                                 unsigned pixel_bytes, uint32_t source,
                                 unsigned coverage)
{
    uint8_t before[4] = {0}, channels[4];
    uint8_t *packed = pixel;
    unsigned packed_bytes = pixel_bytes;
    uint32_t format = gpu->states[0xa10];
    uint32_t destination;
    uint32_t mode = gpu->states[0xa00];
    unsigned alpha, channel;

    if (!coverage) {
        return 0;
    }
    memcpy(before, packed, packed_bytes);
    destination = miband_gpu_pixel_read(packed, format);
    stl_le_p(channels, destination);
    pixel = channels;
    pixel_bytes = (format == 0x103 || format == 0x10a || format == 0x105 || format == 0x106) ? 4 : 3;
    if ((mode & 0x7000) == 0x2000) {
        uint32_t modifier = gpu->states[0xa02];
        uint32_t multiplied = 0;

        for (channel = 0; channel < 4; channel++) {
            unsigned shift = channel * 8;
            unsigned component = ((source >> shift) & 255) *
                                 ((modifier >> shift) & 255);

            multiplied |= ((component + 127) / 255) << shift;
        }
        source = multiplied;
    }
    alpha = source >> 24;
    if ((mode & 0xf00) == 0) {
        /* NONE replaces the destination, including filtered alpha. It must
         * not use SRC_OVER when preparing an offscreen image. */
        miband_gpu_pixel_write(packed, format, source);
        return memcmp(before, packed, packed_bytes) != 0;
    } else if ((mode & 0xf00) == 0x400) {
        /* Coverage interpolates D with D*Sa. Applying coverage to Sa first
         * would erase untouched portions of antialiased mask edges. */
        unsigned factor = 255 * 255 - coverage * (255 - alpha);

        for (channel = 0; channel < pixel_bytes; channel++) {
            pixel[channel] = (pixel[channel] * factor + 32512) / 65025;
        }
    } else {
        unsigned effective_alpha = (alpha * coverage + 127) / 255;

        /* Draw-time CLUT samples (e.g. white 100f0f0f vs ffffffff) and
         * firmware colour fades are already alpha-premultiplied. Multiplying
         * the source RGB by alpha again produces dark edges and wrong fades.
         * Scale premultiplied RGB only by coverage, then SRC_OVER. */
        for (channel = 0; channel < 3; channel++) {
            unsigned source_byte = (source >> (channel * 8)) & 255;
            unsigned value = (source_byte * coverage + pixel[channel] *
                              (255 - effective_alpha) + 127) / 255;

            pixel[channel] = MIN(value, 255);
        }
        if (pixel_bytes == 4) {
            pixel[3] = effective_alpha +
                       (pixel[3] * (255 - effective_alpha) + 127) / 255;
        }
    }
    miband_gpu_pixel_write(packed, format, ldl_le_p(channels));
    return memcmp(before, packed, packed_bytes) != 0;
}

typedef struct GpuPathEdge {
    float x0, y0, x1, y1;
    int direction;
} GpuPathEdge;

typedef struct GpuPathCrossing {
    float x;
    int direction;
} GpuPathCrossing;

typedef struct GpuPathState {
    GArray *edges;
    float current_x, current_y;
    float start_x, start_y;
    bool has_current;
} GpuPathState;

static bool gpu_path_edge(MiBandGpu *gpu, GpuPathState *path,
                          float x0, float y0, float x1, float y1)
{
    GpuPathEdge edge;
    float m00 = gpu_float(gpu->states[0xa40]);
    float m01 = gpu_float(gpu->states[0xa41]);
    float m02 = gpu_float(gpu->states[0xa42]);
    float m10 = gpu_float(gpu->states[0xa43]);
    float m11 = gpu_float(gpu->states[0xa44]);
    float m12 = gpu_float(gpu->states[0xa45]);

    if (path->edges->len >= 8192) {
        return false;
    }
    edge.x0 = m00 * x0 + m01 * y0 + m02;
    edge.y0 = m10 * x0 + m11 * y0 + m12;
    edge.x1 = m00 * x1 + m01 * y1 + m02;
    edge.y1 = m10 * x1 + m11 * y1 + m12;
    if (!isfinite(edge.x0) || !isfinite(edge.y0) ||
        !isfinite(edge.x1) || !isfinite(edge.y1) ||
        fabsf(edge.x0) > 4096 || fabsf(edge.y0) > 4096 ||
        fabsf(edge.x1) > 4096 || fabsf(edge.y1) > 4096) {
        return false;
    }
    edge.direction = edge.y1 > edge.y0 ? 1 : -1;
    g_array_append_val(path->edges, edge);
    return true;
}

static bool gpu_path_close(MiBandGpu *gpu, GpuPathState *path)
{
    bool ok = true;

    if (path->has_current &&
        (path->current_x != path->start_x ||
         path->current_y != path->start_y)) {
        ok = gpu_path_edge(gpu, path, path->current_x, path->current_y,
                           path->start_x, path->start_y);
    }
    path->has_current = false;
    return ok;
}

static float gpu_path_scalar(const uint8_t *data, bool fp32)
{
    return fp32 ? gpu_float(ldl_le_p(data)) : (int16_t)lduw_le_p(data);
}

/* Subdivide in path space, but bound the control hull in screen pixels. A
 * fixed number of curve steps changes accuracy with the firmware matrix.
 * The 1/512 pixel bound is a software geometry tolerance, not a claim about
 * the hardware tessellator's precision. */
static bool gpu_path_curve(MiBandGpu *gpu, GpuPathState *path,
                           const float points[4][2], unsigned count,
                           unsigned depth)
{
    float screen[4][2], work[4][2], left[4][2], right[4][2];
    float dx, dy, length_squared;
    bool flat = true;
    unsigned i, level, coordinate;

    for (i = 0; i < count; i++) {
        screen[i][0] = gpu_float(gpu->states[0xa40]) * points[i][0] +
                       gpu_float(gpu->states[0xa41]) * points[i][1] +
                       gpu_float(gpu->states[0xa42]);
        screen[i][1] = gpu_float(gpu->states[0xa43]) * points[i][0] +
                       gpu_float(gpu->states[0xa44]) * points[i][1] +
                       gpu_float(gpu->states[0xa45]);
        if (!isfinite(screen[i][0]) || !isfinite(screen[i][1]) ||
            fabsf(screen[i][0]) > 4096 || fabsf(screen[i][1]) > 4096) {
            return false;
        }
    }
    dx = screen[count - 1][0] - screen[0][0];
    dy = screen[count - 1][1] - screen[0][1];
    length_squared = dx * dx + dy * dy;
    for (i = 1; i + 1 < count; i++) {
        float px = screen[i][0] - screen[0][0];
        float py = screen[i][1] - screen[0][1];
        float t = length_squared ?
                  CLAMP((px * dx + py * dy) / length_squared, 0.0f, 1.0f) : 0;
        float ex = px - t * dx, ey = py - t * dy;

        if (ex * ex + ey * ey > 1.0f / (512 * 512)) {
            flat = false;
        }
    }
    if (flat) {
        return gpu_path_edge(gpu, path, points[0][0], points[0][1],
                             points[count - 1][0], points[count - 1][1]);
    }
    if (depth >= 16) {
        return false;
    }
    memcpy(work, points, count * sizeof(work[0]));
    memcpy(left[0], work[0], sizeof(left[0]));
    memcpy(right[count - 1], work[count - 1], sizeof(right[0]));
    for (level = 1; level < count; level++) {
        for (i = 0; i < count - level; i++) {
            for (coordinate = 0; coordinate < 2; coordinate++) {
                work[i][coordinate] =
                    (work[i][coordinate] + work[i + 1][coordinate]) * 0.5f;
            }
        }
        memcpy(left[level], work[0], sizeof(left[0]));
        memcpy(right[count - level - 1], work[count - level - 1],
               sizeof(right[0]));
    }
    return gpu_path_curve(gpu, path, left, count, depth + 1) &&
           gpu_path_curve(gpu, path, right, count, depth + 1);
}

/*
 * Path streams are firmware supplied and the exact BES command encoding is
 * still being established.  Keep a bounded trace of rejected paths so that a
 * bad decode cannot be mistaken for a successful full frame.  This is an
 * opt-in diagnostic and has no effect on the rendering result.
 */
static unsigned gpu_path_diag_count;

static bool gpu_path_diag_enabled(void)
{
    if (g_private_get(&gpu_private_render)) {
        return false;
    }
    const char *value = g_getenv("MIBAND_GPU_PATH_DIAG");

    return value && !strcmp(value, "1") && gpu_path_diag_count < 256;
}

static void gpu_path_diag(const char *reason, const GpuPathState *path,
                          size_t offset, unsigned opcode, bool fp32,
                          size_t length)
{
    if (!gpu_path_diag_enabled()) {
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "miband GPU path reject reason=%s off=%#zx opcode=%u "
                  "fp32=%u length=%zu edges=%u current=%u (%g,%g) "
                  "start=(%g,%g)\n", reason, offset, opcode, fp32,
                  length, path->edges ? path->edges->len : 0,
                  path->has_current, path->current_x, path->current_y,
                  path->start_x, path->start_y);
    gpu_path_diag_count++;
}

static bool gpu_path_parse(MiBandGpu *gpu, GpuPathState *path,
                           const uint8_t *data, size_t length, bool fp32)
{
    static const uint8_t count[] = {0, 0, 2, 2, 2, 2, 4, 4, 6, 6, 0};
    size_t offset = 0;
    unsigned scalar_bytes = fp32 ? 4 : 2;

    while (offset + scalar_bytes <= length) {
        unsigned opcode = data[offset];
        float p[6] = {0};
        float x0, y0;
        unsigned n, i;

        if (opcode >= G_N_ELEMENTS(count)) {
            gpu_path_diag("opcode", path, offset, opcode, fp32, length);
            return false;
        }
        n = count[opcode];
        offset += scalar_bytes;
        if ((size_t)n * scalar_bytes > length - offset) {
            gpu_path_diag("truncated", path, offset, opcode, fp32, length);
            return false;
        }
        for (i = 0; i < n; i++) {
            p[i] = gpu_path_scalar(data + offset + i * scalar_bytes, fp32);
        }
        offset += (size_t)n * scalar_bytes;
        if (opcode == 0) {
            return gpu_path_close(gpu, path);
        }
        if (opcode == 1 || opcode == 10) {
            if (!gpu_path_close(gpu, path)) {
                gpu_path_diag("close-edge", path, offset, opcode, fp32,
                              length);
                return false;
            }
            continue;
        }
        x0 = path->current_x;
        y0 = path->current_y;
        if (opcode & 1) {
            for (i = 0; i < n; i += 2) {
                p[i] += x0;
                p[i + 1] += y0;
            }
        }
        if (opcode == 2 || opcode == 3) {
            if (!gpu_path_close(gpu, path)) {
                gpu_path_diag("move-close", path, offset, opcode, fp32,
                              length);
                return false;
            }
            path->current_x = path->start_x = p[0];
            path->current_y = path->start_y = p[1];
            path->has_current = true;
            continue;
        }
        if (!path->has_current) {
            gpu_path_diag("draw-without-move", path, offset, opcode, fp32,
                          length);
            return false;
        }
        if (opcode == 4 || opcode == 5) {
            if (!gpu_path_edge(gpu, path, x0, y0, p[0], p[1])) {
                gpu_path_diag("edge", path, offset, opcode, fp32, length);
                return false;
            }
        } else {
            float points[4][2] = {{x0, y0}};

            for (i = 0; i < n; i += 2) {
                points[i / 2 + 1][0] = p[i];
                points[i / 2 + 1][1] = p[i + 1];
            }
            if (!gpu_path_curve(gpu, path, points, n / 2 + 1, 0)) {
                gpu_path_diag("curve", path, offset, opcode, fp32, length);
                return false;
            }
        }
        path->current_x = p[n - 2];
        path->current_y = p[n - 1];
    }
    return gpu_path_close(gpu, path);
}

static int gpu_path_compare_crossing(const void *a, const void *b)
{
    const GpuPathCrossing *left = a, *right = b;
    return (left->x > right->x) - (left->x < right->x);
}

static unsigned gpu_path_paint_span(MiBandGpu *gpu, uint8_t *row,
                                    uint32_t width, unsigned pixel_bytes,
                                    const uint8_t *coverage, uint32_t color)
{
    unsigned x, changed = 0;

    for (x = 0; x < width; x++) {
        changed += gpu_paint_pixel(gpu, row + (size_t)x * pixel_bytes,
                                    pixel_bytes, color, coverage[x]);
    }
    return changed;
}

static unsigned gpu_path_paint_pattern_span(MiBandGpu *gpu, uint8_t *row,
                                            uint32_t width,
                                            unsigned pixel_bytes,
                                            const uint8_t *coverage,
                                            unsigned y,
                                            const uint8_t *source,
                                            uint32_t source_stride,
                                            uint32_t source_width,
                                            uint32_t source_height,
                                            GpuSourceKind source_kind)
{
    float c0 = gpu_float(gpu->states[0xa18]);
    float c1 = gpu_float(gpu->states[0xa19]);
    float x0 = gpu_float(gpu->states[0xa1c]);
    float x1 = gpu_float(gpu->states[0xa1d]);
    float y0 = gpu_float(gpu->states[0xa20]);
    float y1 = gpu_float(gpu->states[0xa21]);
    unsigned x, changed = 0;

    for (x = 0; x < width; x++) {
        uint32_t color;
        float u, v;

        if (!coverage[x]) {
            continue;
        }
        /* a1c/a1d are the x-step column; a20/a21 are the y-step. */
        u = (c0 + x0 * x + y0 * (float)y) *
            (gpu->texture_texel_coordinates ? 1 : source_width);
        v = (c1 + x1 * x + y1 * (float)y) *
            (gpu->texture_texel_coordinates ? 1 : source_height);
        color = gpu_sample_source(gpu, source, source_stride,
                                   source_width, source_height,
                                   u, v, source_kind);
        changed += gpu_paint_pixel(gpu, row + (size_t)x * pixel_bytes,
                                    pixel_bytes, color, coverage[x]);
    }
    return changed;
}

static bool gpu_draw_path(MiBandGpu *gpu, const uint8_t *data, size_t length)
{
    uint32_t format = gpu->states[0xa10];
    uint32_t width = gpu->states[0xa13] & 0xffff;
    uint32_t height = gpu->states[0xa13] >> 16;
    uint32_t register_stride = gpu->states[0xa12];
    uint32_t path_control = gpu->states[0xa34];
    uint32_t window_origin = gpu->states[0xa39];
    uint32_t window_size = gpu->states[0xa3a];
    uint32_t window_width = window_size & 0xffff;
    uint32_t window_height = window_size >> 16;
    uint32_t clip_x0 = 0, clip_y0 = 0;
    uint32_t clip_x1 = width, clip_y1 = height;
    uint32_t stride;
    unsigned pixel_bytes = miband_gpu_pixel_bytes(format);
    GpuPathState path = {0};
    GpuPathCrossing *crossings;
    uint8_t *target;
    const uint8_t *source = NULL;
    uint32_t source_stride = 0, source_width = 0, source_height = 0;
    GpuSourceKind source_kind = GPU_SOURCE_INDEX8_POINT;
    bool pattern = (gpu->states[0xa00] & 0x7000) != 0;
    bool fp32, ok;
    unsigned y, painted = 0;
    unsigned quality = path_control & 3;
    unsigned sample_grid = quality == 3 ? 4 : quality == 1 ? 2 : 1;

    gpu_dump_draw_inputs(gpu, data, length, false);
    if (!gpu_paint_state_supported(gpu, false)) {
        qemu_log_mask(LOG_UNIMP,
                      "miband GPU path rejected paint state "
                      "mode=%08x global_alpha=%08x\n",
                      gpu->states[0xa00], gpu->states[0xad1]);
        return false;
    }
    /* reference rounds a13's tessellation width to 224 for a 212-pixel RGB565
     * surface (a12=424). Its a39/a3a windows still bound every write to the
     * actual row. Validate the accessed extent, keeping out-of-pitch windows
     * rejected rather than treating the aligned width as the memory pitch. */
    if (window_width && window_height) {
        uint32_t window_x = window_origin & 0xffff;
        uint32_t window_y = window_origin >> 16;

        clip_x0 = MIN(window_x, width);
        clip_y0 = MIN(window_y, height);
        clip_x1 = MIN(window_x + window_width, width);
        clip_y1 = MIN(window_y + window_height, height);
    }
    if (!miband_gpu_target_layout(format, register_stride, clip_x1, &stride) ||
        !width || !height || width > 1024 ||
        height > 1024 || length > 16384) {
        if (gpu_path_diag_enabled()) {
            qemu_log_mask(LOG_UNIMP,
                          "miband GPU path reject reason=target-state "
                          "target=%08x format=%03x size=%ux%u stride=%u "
                          "length=%zu control=%08x\n", gpu->states[0xa11],
                          format, width, height, register_stride, length,
                          path_control);
            gpu_path_diag_count++;
        }
        if (gpu->acknowledged < 32) {
            qemu_log_mask(LOG_UNIMP,
                          "miband GPU path rejected target=%08x format=%03x "
                          "size=%ux%u stride=%u length=%zu\n",
                          gpu->states[0xa11], format, width, height,
                          register_stride, length);
        }
        return false;
    }
    target = gpu_target_ptr(gpu, gpu->states[0xa11], (size_t)stride * height);
    if (!target) {
        if (gpu_path_diag_enabled()) {
            qemu_log_mask(LOG_UNIMP,
                          "miband GPU path reject reason=target-address "
                          "target=%08x bytes=%zu\n", gpu->states[0xa11],
                          (size_t)stride * height);
            gpu_path_diag_count++;
        }
        return false;
    }
    /*
     * Firmware bounds each path tessellation with a39=(x,y), a3a=(w,h).
     * Its half-frame and five-row updates reuse a full-screen clear path:
     * clipping only the geometry would erase the retained pixels outside
     * the update. Captured tessellation-window writes suggest this layout;
     * captured AP commands verify it for (0,240,336,240) and (0,475,336,5).
     * A zero origin is valid. Zero extents have no verified path semantics
     * in the guest traces, so retain target bounds until both are supplied.
     */
    if (pattern) {
        uint32_t source_format = gpu->states[0xa25];
        uint32_t source_size = gpu->states[0xa2f];

        gpu_dump_gradient_source(gpu, data, length);
        source_width = source_size & 0xffffu;
        source_height = source_size >> 16;
        if (!gpu_source_layout(gpu, source_format, gpu->states[0xa2b],
                               source_width, source_height,
                               &source_stride, &source_kind) ||
            !source_width || !source_height ||
            source_width > 4096 || source_height > 4096 ||
            (uint64_t)source_stride * source_height > 16u * 1024u * 1024u) {
            if (gpu_path_diag_enabled()) {
                qemu_log_mask(LOG_UNIMP,
                              "miband GPU pattern path reject source: "
                              "format=%08x address=%08x size=%ux%u "
                              "stride=%u\n", source_format,
                              gpu->states[0xa29], source_width, source_height,
                              gpu->states[0xa2b]);
                gpu_path_diag_count++;
            }
            return false;
        }
        source = gpu_psram_ptr(gpu, gpu->states[0xa29],
                               (size_t)source_stride * source_height);
        if (!source) {
            if (gpu_path_diag_enabled()) {
                qemu_log_mask(LOG_UNIMP,
                              "miband GPU pattern path reject address: "
                              "source=%08x bytes=%zu\n", gpu->states[0xa29],
                              (size_t)source_stride * source_height);
                gpu_path_diag_count++;
            }
            return false;
        }
        if (!gpu_source_content_supported(gpu, source, source_width,
                                           source_kind)) {
            if (gpu_path_diag_enabled()) {
                qemu_log_mask(LOG_UNIMP,
                              "miband GPU pattern path reject gradient: "
                              "format=%08x address=%08x size=%ux%u "
                              "reason=unverified-alpha-or-matrix\n",
                              source_format, gpu->states[0xa29],
                              source_width, source_height);
                gpu_path_diag_count++;
            }
            return false;
        }
    }
    if ((path_control & 0x00300000) == 0x00100000) {
        fp32 = false;
    } else if ((path_control & 0x00300000) == 0x00300000) {
        fp32 = true;
    } else {
        if (gpu_path_diag_enabled()) {
            qemu_log_mask(LOG_UNIMP,
                          "miband GPU path reject reason=scalar-mode "
                          "control=%08x target=%08x\n", path_control,
                          gpu->states[0xa11]);
            gpu_path_diag_count++;
        }
        return false;
    }
    if (quality == 2) {
        /* No firmware capture has requested UPPER quality yet. */
        gpu_path_diag("quality", &path, 0, quality, fp32, length);
        return false;
    }
    path.edges = g_array_sized_new(false, false, sizeof(GpuPathEdge), 128);
    ok = gpu_path_parse(gpu, &path, data, length, fp32);
    if (!ok) {
        if (gpu_path_diag_enabled()) {
            qemu_log_mask(LOG_UNIMP,
                          "miband GPU path parse failed target=%08x "
                          "format=%03x control=%08x length=%zu "
                          "matrix=%08x,%08x,%08x,%08x,%08x,%08x\n",
                          gpu->states[0xa11], format, path_control, length,
                          gpu->states[0xa40], gpu->states[0xa41],
                          gpu->states[0xa42], gpu->states[0xa43],
                          gpu->states[0xa44], gpu->states[0xa45]);
        }
        if (gpu->acknowledged < 32) {
            qemu_log_mask(LOG_UNIMP,
                          "miband GPU path parse failed target=%08x "
                          "format=%03x control=%08x length=%zu\n",
                          gpu->states[0xa11], format, path_control, length);
        }
        g_array_free(path.edges, true);
        return false;
    }
    crossings = g_new(GpuPathCrossing, path.edges->len);
    for (y = clip_y0; y < clip_y1; y++) {
        uint8_t coverage[1024] = {0};
        unsigned sy, x, samples = sample_grid * sample_grid;

        /* Captures request LOW, MEDIUM and HIGH. The mode names suggest 1, 4
         * and 16 samples. These centred square grids are an explicit
         * software candidate; no silicon sample positions are known.
         * Accumulate a whole pixel before painting it so shared contour
         * edges and several covered samples cannot blend it repeatedly. */
        for (sy = 0; sy < sample_grid; sy++) {
            float sample_y = y + (sy + 0.5f) / sample_grid;
            unsigned count = 0, i;
            int winding = 0;
            float span_start = 0;

            for (i = 0; i < path.edges->len; i++) {
                GpuPathEdge *edge = &g_array_index(path.edges, GpuPathEdge, i);

                if ((edge->y0 <= sample_y && edge->y1 > sample_y) ||
                    (edge->y1 <= sample_y && edge->y0 > sample_y)) {
                    crossings[count].x = edge->x0 +
                        (sample_y - edge->y0) *
                        (edge->x1 - edge->x0) / (edge->y1 - edge->y0);
                    crossings[count].direction = edge->direction;
                    count++;
                }
            }
            qsort(crossings, count, sizeof(*crossings),
                  gpu_path_compare_crossing);
            for (i = 0; i < count; i++) {
                int previous = winding;

                winding += (path_control & 0x10) ? 1 : crossings[i].direction;
                if ((path_control & 0x10) && winding == 2) {
                    winding = 0;
                }
                if (!previous && winding) {
                    span_start = crossings[i].x;
                } else if (previous && !winding) {
                    float left = MAX(span_start, (float)clip_x0);
                    float right = MIN(crossings[i].x, (float)clip_x1);
                    int first, last, sx;

                    if (left >= right) {
                        continue;
                    }
                    first = CLAMP((int)ceilf(left * sample_grid - 0.5f),
                                  (int)(clip_x0 * sample_grid),
                                  (int)(clip_x1 * sample_grid));
                    last = CLAMP((int)ceilf(right * sample_grid - 0.5f),
                                 (int)(clip_x0 * sample_grid),
                                 (int)(clip_x1 * sample_grid));
                    for (sx = first; sx < last; sx++) {
                        coverage[sx / sample_grid]++;
                    }
                }
            }
        }
        for (x = clip_x0; x < clip_x1; x++) {
            coverage[x] = (coverage[x] * 255 + samples / 2) / samples;
        }
        /* Retain worker write ownership for each covered run, including
         * partially covered antialiased edge pixels. Uncovered holes are
         * not part of the write set. */
        for (x = clip_x0; x < clip_x1; ) {
            unsigned first;

            while (x < clip_x1 && !coverage[x]) {
                x++;
            }
            first = x;
            while (x < clip_x1 && coverage[x]) {
                x++;
            }
            if (first < x) {
                gpu_note_write(gpu, gpu->states[0xa11] + y * stride +
                                first * pixel_bytes, (x - first) * pixel_bytes);
            }
        }
        if (pattern) {
            painted += gpu_path_paint_pattern_span(
                gpu, target + (size_t)y * stride, width, pixel_bytes,
                coverage, y, source, source_stride,
                source_width, source_height, source_kind);
        } else {
            painted += gpu_path_paint_span(
                gpu, target + (size_t)y * stride, width, pixel_bytes,
                coverage, gpu->states[0xa02]);
        }
    }
    unsigned edge_count = path.edges->len;

    g_free(crossings);
    g_array_free(path.edges, true);
    if (gpu_path_diag_enabled()) {
        qemu_log_mask(LOG_UNIMP,
                      "miband GPU path draw target=%08x control=%08x "
                      "mode=%08x color=%08x fp32=%u length=%zu edges=%u "
                      "painted=%u window=%08x/%08x clip=%u,%u,%u,%u\n",
                      gpu->states[0xa11], path_control, gpu->states[0xa00],
                      gpu->states[0xa02], fp32, length, edge_count,
                      painted, window_origin, window_size,
                      clip_x0, clip_y0, clip_x1, clip_y1);
        gpu_path_diag_count++;
    }
    gpu->path_rendered++;
    return true;
}

static bool gpu_blit_index8(MiBandGpu *gpu, const uint8_t *rectangle)
{
    uint32_t target_size = gpu->states[0xa13];
    uint32_t source_size = gpu->states[0xa2f];
    uint32_t tw = target_size & 0xffff, th = target_size >> 16;
    uint32_t sw = source_size & 0xffff, sh = source_size >> 16;
    uint32_t ts = gpu->states[0xa12], ss = gpu->states[0xa2b];
    uint32_t target_format = gpu->states[0xa10];
    uint32_t source_format = gpu->states[0xa25];
    GpuSourceKind source_kind;
    uint32_t source_stride;
    uint32_t pixel_bytes = miband_gpu_pixel_bytes(target_format);
    uint32_t target_stride;
    int rx = lduw_le_p(rectangle), ry = lduw_le_p(rectangle + 2);
    int rw = lduw_le_p(rectangle + 4), rh = lduw_le_p(rectangle + 6);
    int left = MAX(0, rx), right = MIN((int)tw, rx + rw);
    uint8_t *target, *source;
    float c0, c1, x0, x1, y0, y1;
    int x, y;
    unsigned sampled_alpha = 0, sampled_rgb = 0, changed = 0;

    gpu_dump_draw_inputs(gpu, rectangle, 8, true);
    /* Captured reference offscreen chart clear: solid rectangle, not a texture.
     * Source registers still contain the previous draw and must be ignored. */
    if (gpu->states[0xa00] == 0x10000102) {
        if (!gpu_paint_state_supported(gpu, true) || !tw || !th ||
            tw > 1024 || th > 1024 ||
            !miband_gpu_target_layout(target_format, ts, right, &target_stride)) {
            return false;
        }
        target = gpu_target_ptr(gpu, gpu->states[0xa11], (size_t)target_stride * th);
        if (!target) {
            return false;
        }
        for (y = MAX(0, ry); y < MIN((int)th, ry + rh); y++) {
            if (left < right) {
                gpu_note_write(gpu, gpu->states[0xa11] + y * target_stride +
                               left * pixel_bytes, (right - left) * pixel_bytes);
            }
            for (x = left; x < right; x++) {
                gpu_paint_pixel(gpu, target + (size_t)y * target_stride +
                                x * pixel_bytes, pixel_bytes,
                                gpu->states[0xa02], 255);
            }
        }
        gpu->rendered++;
        return true;
    }
    /* Rectangle bounds, like path windows, describe the accessed columns.
     * Aligned target geometry may exceed the backing row's pixel count. */
    if (!miband_gpu_target_layout(target_format, ts, right, &target_stride) ||
        !gpu_source_layout(gpu, source_format, ss, sw, sh,
                           &source_stride, &source_kind) ||
        !gpu_paint_state_supported(gpu, true) ||
        !tw || !th || !sw || !sh ||
        tw > 1024 || th > 1024 || sw > 1024 || sh > 1024) {
        if (gpu->acknowledged < 32) qemu_log_mask(LOG_UNIMP,
                      "miband GPU INDEX8 rejected state: target=%08x "
                      "source=%08x mode=%08x size=%ux%u/%ux%u "
                      "stride=%u/%u\n", gpu->states[0xa10],
                      gpu->states[0xa25], gpu->states[0xa00],
                      tw, th, sw, sh, ts, ss);
        return false;
    }
    target = gpu_target_ptr(gpu, gpu->states[0xa11],
                            (size_t)target_stride * th);
    source = gpu_psram_ptr(gpu, gpu->states[0xa29],
                           (size_t)source_stride * sh);
    if (!target || !source) {
        if (gpu->acknowledged < 32) qemu_log_mask(LOG_UNIMP,
                      "miband GPU INDEX8 rejected address: target=%08x "
                      "source=%08x\n", gpu->states[0xa11],
                      gpu->states[0xa29]);
        return false;
    }
    if (!gpu_source_content_supported(gpu, source, sw, source_kind)) {
        if (gpu->acknowledged < 32) {
            qemu_log_mask(LOG_UNIMP,
                          "miband GPU rectangle reject gradient: "
                          "format=%08x source=%08x "
                          "reason=unverified-alpha-or-matrix\n",
                          source_format, gpu->states[0xa29]);
        }
        return false;
    }
    if (gpu->acknowledged < 32) {
        int sample_x, sample_y;

        for (sample_y = 0; sample_y < 8; sample_y++) {
            for (sample_x = 0; sample_x < 8; sample_x++) {
                uint32_t color = gpu_source_sample(gpu, source, source_stride,
                    sample_x * sw / 8, sample_y * sh / 8, source_kind);
                sampled_alpha += (color >> 24) != 0;
                sampled_rgb += (color & 0x00ffffff) != 0;
            }
        }
    }
    c0 = gpu_float(gpu->states[0xa18]);
    c1 = gpu_float(gpu->states[0xa19]);
    x0 = gpu_float(gpu->states[0xa1c]);
    x1 = gpu_float(gpu->states[0xa1d]);
    y0 = gpu_float(gpu->states[0xa20]);
    y1 = gpu_float(gpu->states[0xa21]);
    if (gpu_float(gpu->states[0xa1a]) != 1.0f ||
        gpu_float(gpu->states[0xa1e]) != 0.0f ||
        gpu_float(gpu->states[0xa22]) != 0.0f) {
        if (gpu->acknowledged < 32) qemu_log_mask(LOG_UNIMP,
                      "miband GPU INDEX8 rejected matrix: c2=%08x "
                      "x2=%08x y2=%08x\n", gpu->states[0xa1a],
                      gpu->states[0xa1e], gpu->states[0xa22]);
        return false;
    }
    for (y = MAX(0, ry); y < MIN((int)th, ry + rh); y++) {
        if (left < right) {
            gpu_note_write(gpu, gpu->states[0xa11] + y * target_stride +
                            left * pixel_bytes, (right - left) * pixel_bytes);
        }
        for (x = left; x < right; x++) {
            float u = (c0 + x0 * x + y0 * y) *
                      (gpu->texture_texel_coordinates ? 1 : sw);
            float v = (c1 + x1 * x + y1 * y) *
                      (gpu->texture_texel_coordinates ? 1 : sh);
            uint32_t src_color = gpu_sample_source(gpu, source, source_stride,
                                                   sw, sh, u, v, source_kind);
            uint8_t *dst = target + (size_t)y * target_stride +
                           (size_t)x * pixel_bytes;
            changed += gpu_paint_pixel(gpu, dst, pixel_bytes,
                                        src_color, 255);
        }
    }
    if (gpu->acknowledged < 32) {
        qemu_log_mask(LOG_UNIMP,
                      "miband GPU blit target=%08x source=%08x "
                      "format=%08x/%08x source_size=%ux%u rect=%d,%d,%d,%d "
                      "sample_alpha=%u/64 sample_rgb=%u/64 changed=%u\n",
                      gpu->states[0xa11], gpu->states[0xa29],
                      target_format, source_format, sw, sh,
                      rx, ry, rw, rh, sampled_alpha, sampled_rgb, changed);
    }
    gpu->rendered++;
    return true;
}

static bool gpu_run_stream(MiBandGpu *gpu, const uint8_t *commands,
                           size_t length, unsigned depth, size_t *budget,
                           unsigned *paths, unsigned *blits)
{
    size_t offset = 0;

    if (!commands || !length || length > GPU_MAX_COMMAND_BYTES ||
        length > *budget) {
        return false;
    }
    *budget -= length;
    while (offset + 8 <= length) {
        uint32_t command = ldl_le_p(commands + offset);
        uint32_t opcode = command >> 28;

        if (opcode == 3) {
            uint32_t count = (command >> 16) & 0xfff;
            uint32_t reg = command & 0xffff;
            size_t bytes = ((size_t)count + 2) / 2 * 8;
            unsigned i;

            if (!count || reg + count > ARRAY_SIZE(gpu->states) ||
                bytes > length - offset) {
                return false;
            }
            for (i = 0; i < count; i++) {
                gpu->states[reg + i] =
                    ldl_le_p(commands + offset + 4 + i * 4);
            }
            offset += bytes;
        } else if (opcode == 4) {
            uint32_t units = command & 0x0fffffff;
            size_t bytes = ((size_t)units + 1) * 8;

            if (!units || bytes > length - offset) {
                return false;
            }
            if (units == 1) {
                if (!gpu_blit_index8(gpu, commands + offset + 8)) {
                    if (gpu->acknowledged < 32) qemu_log_mask(LOG_UNIMP,
                                  "miband GPU rejected rectangle at "
                                  "command offset %#zx\n", offset);
                    return false;
                }
                (*blits)++;
            } else if (gpu_draw_path(gpu, commands + offset + 8,
                                     (size_t)units * 8)) {
                (*blits)++;
            } else {
                (*paths)++;
            }
            offset += bytes;
        } else if (opcode == 0) {
            return depth == 0;
        } else if (opcode == 1 || opcode == 2) {
            offset += 8;
        } else if (opcode == 6) {
            uint32_t units = command & 0x0fffffff;
            uint32_t address = ldl_le_p(commands + offset + 4);
            size_t bytes = (size_t)units * 8;
            const uint8_t *called;

            if (!units || bytes > GPU_MAX_COMMAND_BYTES ||
                depth >= GPU_MAX_CALL_DEPTH) {
                return false;
            }
            called = gpu_psram_ptr(gpu, address, bytes);
            if (!gpu_run_stream(gpu, called, bytes, depth + 1, budget,
                                paths, blits)) {
                if (gpu->acknowledged < 32) qemu_log_mask(LOG_UNIMP,
                              "miband GPU rejected CALL address=%08x "
                              "bytes=%zu depth=%u\n", address, bytes,
                              depth + 1);
                return false;
            }
            offset += 8;
        } else if (opcode == 7) {
            return depth != 0;
        } else {
            if (gpu->acknowledged < 32) qemu_log_mask(LOG_UNIMP,
                          "miband GPU rejected opcode %u at offset %#zx\n",
                          opcode, offset);
            return false;
        }
    }
    return depth == 0;
}

static bool gpu_run_command(MiBandGpu *gpu)
{
    size_t length = (size_t)gpu->cmd_size * 8;
    const uint8_t *commands = gpu_psram_ptr(gpu, gpu->cmd_address, length);
    size_t budget = GPU_MAX_EXECUTED_BYTES;
    unsigned paths = 0, blits = 0;
    bool accepted;

    accepted = gpu_run_stream(gpu, commands, length, 0, &budget, &paths, &blits);
    if (paths || !accepted) {
        /* A parser success is not a complete rendering. Keep this visible
         * even after the bounded verbose path diagnostics are exhausted. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "miband GPU incomplete rendering cmd=%08x target=%08x "
                      "skipped=%u accepted=%u\n",
                      gpu->cmd_address, gpu->states[0xa11], paths, accepted);
    }
    gpu_aggressive_dump(gpu, commands, length, accepted, paths);
    if (!accepted || (gpu->memory_ptr && paths)) {
        return false;
    }
    if (gpu->acknowledged < 16) {
        qemu_log_mask(LOG_UNIMP,
                      "miband GPU INDEX8 subset: %u blits, %u paths skipped, "
                      "target=%08x rendering_complete=%u\n",
                      blits, paths, gpu->states[0xa11], !paths);
    }
    if (gpu->scanout_bridge && gpu->lcdc) {
        uint32_t target_size = gpu->states[0xa13];
        uint32_t width = target_size & 0xffff;
        uint32_t height = target_size >> 16;
        uint32_t format = gpu->states[0xa10];
        uint32_t address = gpu->states[0xa11];
        uint32_t stride;
        bool ap_ram_target = address >= AP_RAM_BASE &&
                             address - AP_RAM_BASE < gpu->ram_size;

        if (!ap_ram_target && width == gpu->lcdc->width && height == gpu->lcdc->height &&
            miband_gpu_target_layout(format, gpu->states[0xa12], width,
                                       &stride)) {
            uint8_t *source = gpu_target_ptr(gpu, gpu->states[0xa11],
                                              (size_t)stride * height);
            if (source) {
                miband_lcdc_queue_gpu_frame(gpu->lcdc, source,
                                             gpu->states[0xa11], format,
                                             stride, paths);
            }
        }
    }
    return true;
}

static int gpu_compare_range(const void *a, const void *b)
{
    const MiBandDisplayRange *left = a, *right = b;
    if (left->block != right->block) {
        return (left->block > right->block) - (left->block < right->block);
    }
    return (left->offset > right->offset) - (left->offset < right->offset);
}

bool miband_gpu_render_private(MiBandGpu *gpu, GArray *writes)
{
    GpuPrivateRender render = {
        .writes = writes,
        .inputs = g_array_new(false, false, sizeof(MiBandDisplayRange)),
    };
    size_t length = (size_t)gpu->cmd_size * 8;
    size_t budget = GPU_MAX_EXECUTED_BYTES;
    unsigned paths = 0, blits = 0, i, j;
    bool ok;
    const uint8_t *commands;

    assert(!g_private_get(&gpu_private_render));
    g_private_set(&gpu_private_render, &render);
    commands = gpu_psram_ptr(gpu, gpu->cmd_address, length);
    ok = gpu_run_stream(gpu, commands, length, 0, &budget, &paths, &blits);
    /* Unsupported path drawing is a rejection here, even where the existing
     * diagnostic synchronous subset historically acknowledges skipped paths. */
    ok &= !paths && !render.invalid;
    g_array_sort(writes, gpu_compare_range);
    g_array_sort(render.inputs, gpu_compare_range);
    i = j = 0;
    while (ok && i < writes->len && j < render.inputs->len) {
        MiBandDisplayRange *w = &g_array_index(writes, MiBandDisplayRange, i);
        MiBandDisplayRange *r = &g_array_index(render.inputs,
                                              MiBandDisplayRange, j);
        if (miband_display_ranges_overlap(w, r)) {
            ok = false;
        } else if (gpu_compare_range(w, r) < 0) {
            i++;
        } else {
            j++;
        }
    }
    g_private_set(&gpu_private_render, NULL);
    g_array_free(render.inputs, true);
    return ok;
}

void miband_gpu_set_worker(MiBandGpu *gpu, CPUState *cpu, bool requested)
{
    if (!gpu->async) {
        gpu->async = g_new0(struct MiBandGpuAsync, 1);
        gpu->async->generation = 1;
    }
    gpu->async->cpu = cpu;
    gpu->async->requested = requested;
    if (requested && !gpu->async->worker) {
        gpu->async->worker = miband_gpu_worker_new();
    }
    if (requested) {
        qemu_log_mask(LOG_UNIMP,
                      "miband GPU worker requested: live admission GATED; "
                      "driver fence/input lifetime/target ownership not all "
                      "verified; synchronous fallback, replay worker ready\n");
    }
}

void miband_gpu_async_reset(MiBandGpu *gpu)
{
    if (gpu->async) {
        gpu->async->generation++;
        if (gpu->async->worker) {
            miband_gpu_worker_reset(gpu->async->worker);
        }
    }
    gpu->intr_status = 0;
    if (gpu->memory_ptr && gpu->irq) {
        qemu_set_irq(gpu->irq, 0);
    }
}

static int gpu_compare_duration(const void *a, const void *b)
{
    uint64_t left = *(const uint64_t *)a, right = *(const uint64_t *)b;
    return (left > right) - (left < right);
}

void miband_gpu_cleanup(MiBandGpu *gpu)
{
    unsigned count;
    if (!gpu->async) {
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "miband GPU worker stats: admitted=0 fallback-unverified=%"
                  PRIu64 " sync-count=%" PRIu64 " sync-total-ns=%" PRIu64
                  " sync-max-ns=%" PRIu64 "\n",
                  gpu->async->fallback_unverified, gpu->async->sync_count,
                  gpu->async->sync_total_ns, gpu->async->sync_max_ns);
    count = MIN(gpu->async->sync_count,
                ARRAY_SIZE(gpu->async->sync_samples));
    if (count) {
        qsort(gpu->async->sync_samples, count, sizeof(uint64_t),
              gpu_compare_duration);
        qemu_log_mask(LOG_UNIMP,
                      "miband GPU sync-duration recent-samples=%u "
                      "p50-ns=%" PRIu64 " p95-ns=%" PRIu64
                      " p99-ns=%" PRIu64 "\n", count,
                      gpu->async->sync_samples[(count * 50 - 1) / 100],
                      gpu->async->sync_samples[(count * 95 - 1) / 100],
                      gpu->async->sync_samples[(count * 99 - 1) / 100]);
    }
    miband_gpu_worker_free(gpu->async->worker);
    g_free(gpu->async);
    gpu->async = NULL;
}

bool miband_gpu_pending_memory(uint32_t address, size_t length)
{
    /* All guest submissions are still synchronous. Isolated private replay
     * jobs have no guest-memory dependency and cannot block DMA/LCDC. */
    return false;
}

void miband_gpu_init(MiBandGpu *gpu, uint8_t *psram, uint32_t psram_size,
                     uint8_t *ram, uint32_t ram_size,
                     bool ack_no_render, bool render_index8)
{
    memset(gpu, 0, sizeof(*gpu));
    gpu->base = GPU_BASE;
    gpu->chip_id = 0x265;
    gpu->revision = 0x2002;
    gpu->cid = 0x424;
    gpu->psram = psram;
    gpu->psram_size = psram_size;
    gpu->ram = ram;
    gpu->ram_size = ram_size;
    gpu->ack_no_render = ack_no_render;
    gpu->render_index8 = render_index8;
}

void miband_gpu_set_bus(MiBandGpu *gpu, hwaddr base, uint32_t chip_id,
                        uint32_t revision, uint32_t cid, uint32_t eco_id,
                        void *opaque,
                        uint8_t *(*memory_ptr)(void *, uint32_t, size_t, bool))
{
    gpu->base = base;
    gpu->chip_id = chip_id;
    gpu->revision = revision;
    gpu->cid = cid;
    gpu->eco_id = eco_id;
    gpu->memory_opaque = opaque;
    gpu->memory_ptr = memory_ptr;
}

void miband_gpu_connect_irq(MiBandGpu *gpu, qemu_irq irq)
{
    gpu->irq = irq;
}

void miband_gpu_connect_lcdc(MiBandGpu *gpu, MiBandLcdc *lcdc,
                             bool scanout_bridge)
{
    gpu->lcdc = lcdc;
    gpu->scanout_bridge = scanout_bridge;
}

bool miband_gpu_read(MiBandGpu *gpu, hwaddr address, unsigned size,
                     uint64_t *result)
{
    if (size != sizeof(uint32_t)) {
        return false;
    }
    if (address == gpu->base + GPU_CLOCK_CONTROL) {
        *result = gpu->clock_control;
        return true;
    }
    if (address == gpu->base + GPU_IDLE) {
        *result = (gpu->clock_control & (GPU_SOFT_RESET | GPU_ISOLATE)) ?
                  0 : GPU_IDLE_STATE;
        return true;
    }
    if (address == gpu->base + GPU_INTR_STATUS) {
        *result = gpu->intr_status;
        if (gpu->intr_trace_events++ < 64) {
            qemu_log_mask(LOG_UNIMP,
                          "miband GPU interrupt status read=%08x enable=%08x "
                          "completed=%u\n", gpu->intr_status,
                          gpu->intr_enable, gpu->acknowledged);
        }
        gpu->intr_status = 0;
        if (gpu->memory_ptr && gpu->irq) {
            qemu_set_irq(gpu->irq, 0);
        }
        return true;
    }
    if (address == gpu->base + GPU_INTR_ENABLE) {
        *result = gpu->intr_enable;
        return true;
    }
    if (address == gpu->base + GPU_CMDBUF_ADDRESS) {
        *result = gpu->cmd_address;
        return true;
    }
    if (address == gpu->base + GPU_CMDBUF_SIZE) {
        *result = gpu->cmd_size;
        return true;
    }
    if (address == gpu->base + GPU_CHIP_ID) {
        *result = gpu->chip_id;
        return true;
    }
    if (address == gpu->base + GPU_REVISION) {
        *result = gpu->revision;
        return true;
    }
    if (address == gpu->base + GPU_CID) {
        *result = gpu->cid;
        return true;
    }
    if (address == gpu->base + GPU_ECO_ID) {
        *result = gpu->eco_id;
        return true;
    }
    return false;
}

bool miband_gpu_write(MiBandGpu *gpu, hwaddr address, unsigned size,
                      uint64_t value)
{
    if (size != sizeof(uint32_t)) {
        return false;
    }
    switch (address - gpu->base) {
    case GPU_CLOCK_CONTROL:
        if (address != gpu->base + GPU_CLOCK_CONTROL) {
            return false;
        }
        if ((value & (GPU_SOFT_RESET | GPU_ISOLATE)) &&
            !(gpu->clock_control & (GPU_SOFT_RESET | GPU_ISOLATE))) {
            miband_gpu_async_reset(gpu);
        }
        gpu->clock_control = value;
        return true;
    case GPU_INTR_ENABLE:
        gpu->intr_enable = value;
        if (gpu->memory_ptr && gpu->irq) {
            qemu_set_irq(gpu->irq,
                         (gpu->intr_status & gpu->intr_enable) != 0);
        }
        if (gpu->intr_trace_events++ < 64) {
            qemu_log_mask(LOG_UNIMP,
                          "miband GPU interrupt enable=%08" PRIx64 "\n",
                          value);
        }
        return true;
    case GPU_CMDBUF_ADDRESS:
        gpu->cmd_address = value;
        return true;
    case GPU_CMDBUF_SIZE:
        gpu->cmd_size = value;
        if ((gpu->ack_no_render || gpu->render_index8) &&
            gpu->irq && (gpu->memory_ptr || gpu->intr_enable) &&
            value > 0 && value <= 0x20000 &&
            (gpu->memory_ptr ?
                (value * 8 <= GPU_MAX_COMMAND_BYTES &&
                 gpu_psram_ptr(gpu, gpu->cmd_address, value * 8) != NULL) :
                (gpu->cmd_address >= 0x38000000 &&
                 gpu->cmd_address < 0x3a000000 &&
                 (uint64_t)gpu->cmd_address + value * 8 <= 0x3a000000))) {
            bool rendered_ok = true;
            if (gpu->render_index8) {
                int64_t begin = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
                if (gpu->async && gpu->async->requested) {
                    /* No option or IRQ-enable bit can establish ownership. */
                    gpu->async->fallback_unverified++;
                }
                rendered_ok = gpu_run_command(gpu);
                if (gpu->async) {
                    uint64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) -
                                       begin;
                    gpu->async->sync_count++;
                    gpu->async->sync_total_ns += elapsed;
                    gpu->async->sync_max_ns = MAX(gpu->async->sync_max_ns,
                                                  elapsed);
                    gpu->async->sync_samples[(gpu->async->sync_count - 1) %
                        ARRAY_SIZE(gpu->async->sync_samples)] = elapsed;
                }
            }
            if (!rendered_ok) {
                if (gpu->acknowledged < 32) qemu_log_mask(LOG_UNIMP,
                              "miband GPU unsupported command at %08x; "
                              "completion %s (not rendered)\n",
                              gpu->cmd_address,
                              gpu->ack_no_render ? "diagnostically allowed" :
                              "withheld");
                if (!gpu->ack_no_render) {
                    return true;
                }
            }
            gpu->intr_status = 1;
            gpu->acknowledged++;
            if (gpu->acknowledged <= 16) {
                qemu_log_mask(LOG_UNIMP,
                              "miband GPU diagnostic completion %u: "
                              "cmd=%08x bytes=%" PRIu64
                              " enable=%08x (%s)\n",
                              gpu->acknowledged, gpu->cmd_address, value * 8,
                              gpu->intr_enable,
                              gpu->render_index8 ? "INDEX8 subset" :
                              "not rendered");
            }
            if (gpu->memory_ptr) {
                qemu_set_irq(gpu->irq,
                             (gpu->intr_status & gpu->intr_enable) != 0);
            } else {
                qemu_irq_pulse(gpu->irq);
            }
        }
        return true;
    default:
        return false;
    }
}
