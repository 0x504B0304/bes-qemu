/* Host backend selection; independent of any BES integration model. */
#ifndef MIBAND_HCI_BACKEND_H
#define MIBAND_HCI_BACKEND_H
#include <stdbool.h>
#include "qapi/error.h"

typedef enum MiBandHciBackendKind {
    MIBAND_HCI_SYNTHETIC,
    MIBAND_HCI_SERIAL,
    MIBAND_HCI_LINUX_RAW,
} MiBandHciBackendKind;

typedef struct MiBandHciBackendConfig {
    MiBandHciBackendKind kind;
    char *path;
    unsigned baud, device;
    bool dual_mode;
} MiBandHciBackendConfig;

/* Initializes the output even on failure; clear it after each call. */
bool miband_hci_backend_parse(const char *spec, MiBandHciBackendConfig *config,
                              Error **errp);
void miband_hci_backend_clear(MiBandHciBackendConfig *config);
#endif
