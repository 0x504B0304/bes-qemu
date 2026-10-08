/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Startup-only configuration ABI. All execution remains in native QEMU models. */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qlist.h"
#include "qapi/qmp/qnum.h"
#include "qapi/qmp/qbool.h"
#include "qapi/qmp/qstring.h"
#include "qapi/qmp/qjson.h"
#include "bes_runtime_config.h"

static uint32_t synthetic_curve[101];
MiBandBoardConfig miband_boards[1] = {{
    .id = "synthetic", .width = 128, .height = 128, .framebuffer_bpp = 16,
    .charger_bus = 0, .panel_te_gpio = 0, .battery_channel = 0,
    .thermistor_mask = 2, .thermistor_mv = 900,
    .battery_divider_uv_per_mv = 3000, .curve_uv = synthetic_curve,
    .touch_model = "inferred-report", .touch_bus = 5, .touch_address = 32,
    .touch_irq_gpio = 1, .touch_chip_id = 1, .panel_id = 0,
    .config_sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
    .nand_bus = 1, .nand = {2048, 64, 64, 262144, {0, 0, 0}},
    .charger_probe_revision = 7, .touch_double_tap_status = 1,
    .touch_reset_gpio = -1,
}};
MiBandFirmwareConfig miband_firmwares[1] = {{
    .id = "synthetic", .board = "synthetic",
    .config_sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
    .storage_sha256 = "0000000000000000000000000000000000000000000000000000000000000000",
    .boot_sp = MIBAND_DEFAULT_BOOT_SP, .boot_pc = MIBAND_DEFAULT_BOOT_PC,
    .nor_size = 0x1000000, .resource_page_count = 1, .resource_format = "romfs",
    .ble_service_uuid = "1800", .ble_tx_uuid = "2a00", .ble_rx_uuid = "2a01",
    .miwear_service_uuid = "1801", .gattc_service_uuid = "1802",
    .ble_mtu = 23, .ble_chunk_size = 20, .mass_slice_length = 1024,
}};
MiBandAudioProfile miband_audio_profiles[1] = {{.id = ""}};
MiBandBindingUiProfile miband_binding_ui_profiles[1] = {{.id = ""}};
MiBandBindingAuthProfile miband_binding_auth_profiles[1] = {{.id = ""}};
static bool locked;
static char *config_path;

static void require(bool valid, const char *what)
{
    if (!valid) {
        error_report("hardware-config: invalid %s", what);
        exit(1);
    }
}
static QObject *required(QDict *d, const char *key)
{
    QObject *v = qdict_get(d, key);
    require(v != NULL, key);
    return v;
}
static QDict *read_object(QObject *o, size_t count)
{
    QDict *d = qobject_to(QDict, o);
    require(d && qdict_size(d) == count, "object fields");
    return d;
}
static int64_t read_int(QObject *o, int64_t low, int64_t high)
{
    QNum *n = qobject_to(QNum, o);
    int64_t v = 0;
    require(n && qnum_get_try_int(n, &v), "integer type");
    require(v >= low && v <= high, "integer range");
    return v;
}
static const char *read_str(QObject *o)
{
    QString *s = qobject_to(QString, o);
    require(s && strlen(qstring_get_str(s)) > 0 && strlen(qstring_get_str(s)) <= 128,
            "string length/type");
    return g_strdup(qstring_get_str(s));
}
static const char *read_hash(QObject *o)
{
    const char *s = read_str(o);
    require(strlen(s) == 64 && strspn(s, "0123456789abcdef") == 64, "SHA256");
    return s;
}
static bool read_bool(QObject *o)
{
    QBool *b = qobject_to(QBool, o);
    require(b != NULL, "boolean type");
    return qbool_get_bool(b);
}
static QList *read_array(QObject *o, size_t low, size_t high)
{
    QList *l = qobject_to(QList, o);
    require(l && qlist_size(l) >= low && qlist_size(l) <= high, "array size/type");
    return l;
}
#include "bes_runtime_decode.inc"

static void load_config(Object *obj, const char *path, Error **errp)
{
    g_autofree char *text = NULL;
    gsize size;
    if (locked || config_path) {
        error_setg(errp, "hardware-config is startup-only and may be set once");
        return;
    }
    if (!g_file_get_contents(path, &text, &size, NULL) || size > 262144) {
        error_setg(errp, "cannot read hardware-config (maximum 256 KiB)");
        return;
    }
    QObject *root = qobject_from_json(text, errp);
    if (!root) {
        return;
    }
    QDict *d = qobject_to(QDict, root);
    require(d != NULL, "root object");
    require(read_int(required(d, "abi"), 1, 1) == 1, "ABI");
    const QDictEntry *entry;
    for (entry = qdict_first(d); entry; entry = qdict_next(d, entry)) {
        require(!strcmp(entry->key, "abi") || !strcmp(entry->key, "board") ||
                !strcmp(entry->key, "firmware") || !strcmp(entry->key, "audio") ||
                !strcmp(entry->key, "binding_ui") || !strcmp(entry->key, "binding_auth"),
                "unknown section");
    }
#define LOAD(key, type, storage) \
    if (qdict_haskey(d, key)) { decode_##type(qdict_get(d, key), &storage[0]); }
    LOAD("board", MiBandBoardConfig, miband_boards);
    LOAD("firmware", MiBandFirmwareConfig, miband_firmwares);
    LOAD("audio", MiBandAudioProfile, miband_audio_profiles);
    LOAD("binding_ui", MiBandBindingUiProfile, miband_binding_ui_profiles);
    LOAD("binding_auth", MiBandBindingAuthProfile, miband_binding_auth_profiles);
#undef LOAD
    MiBandBoardConfig *b = &miband_boards[0];
    MiBandFirmwareConfig *f = &miband_firmwares[0];
    if (qdict_haskey(d, "board")) {
        require(b->framebuffer_bpp == 16 || b->framebuffer_bpp == 24 || b->framebuffer_bpp == 32, "pixel format");
        require(!strcmp(b->touch_model, "zinitix") || !strcmp(b->touch_model, "inferred-report"), "touch model");
        require(b->nand.spare_size == 64 || b->nand.spare_size == 128, "NAND spare size");
        require(b->panel_te_gpio != b->touch_irq_gpio && b->touch_reset_gpio != (int)b->panel_te_gpio &&
                b->touch_reset_gpio != (int)b->touch_irq_gpio, "GPIO overlap");
        for (unsigned i = 1; i < 101; i++) {
            require(b->curve_uv[i-1] > b->curve_uv[i], "descending battery curve");
        }
        object_property_set_str(obj, "board-profile", b->id, &error_fatal);
    }
    if (qdict_haskey(d, "firmware")) {
        require(qdict_haskey(d, "board") && !strcmp(b->id, f->board), "board binding");
        require(!(f->boot_sp & 7) && (f->boot_pc & 1) && !(f->flash_offset & 4095), "boot vectors");
        require(f->nor_address <= 0x1000000 - f->nor_size &&
                (int64_t)f->nor_image_offset + f->flash_offset == f->nor_address, "NOR range");
        require(f->resource_first_page <= b->nand.page_count &&
                f->resource_page_count <= b->nand.page_count - f->resource_first_page, "resource range");
        require(f->ble_chunk_size <= f->ble_mtu - 3, "BLE chunk size");
        require(!strcmp(f->resource_format, "romfs") || !strcmp(f->resource_format, "yaffs2-oob") ||
                !strcmp(f->resource_format, "yaffs2-inband"), "resource format");
        object_property_set_str(obj, "firmware-profile", f->id, &error_fatal);
    }
    config_path = g_strdup(path);
    qobject_unref(root);
}
static char *get_config(Object *obj, Error **errp)
{
    return g_strdup(config_path ? config_path : "");
}
void bes_runtime_config_property(ObjectClass *oc)
{
    object_class_property_add_str(oc, "hardware-config", get_config, load_config);
}
void bes_runtime_config_lock(void)
{
    for (unsigned i = 0; i < 101; i++) {
        synthetic_curve[i] = 4300000 - i * 10000;
    }
    locked = true;
}
