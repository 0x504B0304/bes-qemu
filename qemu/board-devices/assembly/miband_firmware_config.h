#ifndef MIBAND_FIRMWARE_CONFIG_H
#define MIBAND_FIRMWARE_CONFIG_H
#include <stdint.h>
#include <stdbool.h>
typedef struct MiBandFirmwareConfig {
    const char *id, *board, *config_sha256;
    const char *storage_sha256;
    const char *ble_service_uuid;
    const char *ble_tx_uuid;
    const char *ble_rx_uuid;
    const char *miwear_service_uuid;
    const char *gattc_service_uuid;
    uint32_t boot_sp, boot_pc;
    uint32_t nor_address;
    int32_t nor_image_offset;
    uint32_t nor_size;
    uint32_t resource_first_page, resource_page_count;
    uint16_t ble_mtu, ble_chunk_size;
    uint32_t mass_slice_length;
    const uint8_t *mass_types;
    uint8_t mass_type_count;
    uint32_t flash_offset;
    const char *resource_format;
    bool legacy_journal;
} MiBandFirmwareConfig;
#endif
