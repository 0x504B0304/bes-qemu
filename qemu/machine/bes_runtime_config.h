/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef BES_RUNTIME_CONFIG_H
#define BES_RUNTIME_CONFIG_H
#include "miband_board_config.h"
#include "miband_firmware_config.h"
#include "miband_audio_adapter.h"
#include "miband_binding_ui.h"
#include "miband_binding_auth.h"
/* One machine per process. Parsed once, immutable for the VM lifetime. */
extern MiBandBoardConfig miband_boards[1];
extern MiBandFirmwareConfig miband_firmwares[1];
extern MiBandAudioProfile miband_audio_profiles[1];
extern MiBandBindingUiProfile miband_binding_ui_profiles[1];
extern MiBandBindingAuthProfile miband_binding_auth_profiles[1];
void bes_runtime_config_property(ObjectClass *oc);
void bes_runtime_config_lock(void);
#define MIBAND_DEFAULT_FIRMWARE_ID "synthetic"
#define MIBAND_DEFAULT_BOOT_SP 0x20010000u
#define MIBAND_DEFAULT_BOOT_PC 0x0c000001u
#endif
