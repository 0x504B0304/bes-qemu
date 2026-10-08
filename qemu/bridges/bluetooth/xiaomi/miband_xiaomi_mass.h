#ifndef MIBAND_XIAOMI_MASS_H
#define MIBAND_XIAOMI_MASS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "miband_xiaomi_protocol.h"

typedef struct MiBandXiaomiMassReceiver MiBandXiaomiMassReceiver;

MiBandXiaomiMassReceiver *miband_xiaomi_mass_receiver_new(void);
void miband_xiaomi_mass_receiver_free(MiBandXiaomiMassReceiver *receiver);
void miband_xiaomi_mass_receiver_reset(MiBandXiaomiMassReceiver *receiver);

/* Accept one L2 mass write body: total block count, current block (both LE),
 * followed by the mass fragment. Blocks are deliberately required in order;
 * the firmware's resume field can restart a new receiver at block one. */
bool miband_xiaomi_mass_receiver_push(MiBandXiaomiMassReceiver *receiver,
                                      const uint8_t *body, size_t length);
bool miband_xiaomi_mass_receiver_complete(const MiBandXiaomiMassReceiver *receiver);
const MiBandXiaomiMass *miband_xiaomi_mass_receiver_result(
    const MiBandXiaomiMassReceiver *receiver);
const uint8_t *miband_xiaomi_mass_receiver_data(
    const MiBandXiaomiMassReceiver *receiver, size_t *length);

#endif
