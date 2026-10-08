/* Generic offline HCI controller responses; no BES MMIO or guest memory. */
#ifndef MIBAND_HCI_SYNTHETIC_H
#define MIBAND_HCI_SYNTHETIC_H
#include <stdbool.h>
#include <stdint.h>

typedef struct MiBandHciSynthetic MiBandHciSynthetic;
/* Identity is supplied by the controller adapter, including vendor updates.
 * These arrays must remain valid for the duration of a command call. */
typedef struct MiBandHciSyntheticIdentity {
    const uint8_t *address;             /* six bytes */
    const uint8_t *features;            /* eight bytes */
    const uint8_t *extended_features;   /* eight bytes */
} MiBandHciSyntheticIdentity;
typedef struct MiBandHciSyntheticResult {
    uint8_t status;
    uint8_t params[255];
    unsigned params_len;
} MiBandHciSyntheticResult;

MiBandHciSynthetic *miband_hci_synthetic_new(const char *local_name);
void miband_hci_synthetic_free(MiBandHciSynthetic *synthetic);
bool miband_hci_synthetic_expected_length(uint16_t opcode, unsigned length);
/* Returns false for unknown standard commands (status 0x01). Invalid command
 * shapes return status 0x12. BES vendor commands belong to the SoC adapter. */
bool miband_hci_synthetic_command(MiBandHciSynthetic *synthetic,
    const MiBandHciSyntheticIdentity *identity, const uint8_t *data,
    unsigned size, MiBandHciSyntheticResult *result);
/* Build a deterministic LE Connection Complete event for the configured
 * synthetic peer. The controller adapter owns mailbox delivery and handle
 * lifetime; this bridge only encodes the standard HCI event. */
unsigned miband_hci_synthetic_connection_complete(const uint8_t *command,
                                                  unsigned size,
                                                  uint16_t handle,
                                                  uint8_t *event);
/* Preserve the legacy offline ACL acknowledgement for callers that have not
 * enabled the synthetic peer procedure. */
unsigned miband_hci_synthetic_acl(const uint8_t *data, unsigned size,
                                  uint8_t *event);
#endif
