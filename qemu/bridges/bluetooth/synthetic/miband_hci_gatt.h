/* Small ATT server used by the offline synthetic HCI peer. */
#ifndef MIBAND_HCI_GATT_H
#define MIBAND_HCI_GATT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct MiBandHciGatt MiBandHciGatt;
typedef void (*MiBandHciGattWriteCallback)(MiBandHciGatt *gatt, void *opaque,
                                            uint16_t attribute,
                                            const uint8_t *value, size_t length,
                                            bool command);

MiBandHciGatt *miband_hci_gatt_new(uint16_t server_mtu);
void miband_hci_gatt_free(MiBandHciGatt *gatt);
void miband_hci_gatt_connect(MiBandHciGatt *gatt, uint16_t handle);
void miband_hci_gatt_disconnect(MiBandHciGatt *gatt, uint16_t handle);
void miband_hci_gatt_set_characteristics(MiBandHciGatt *gatt,
                                          const char *tx_uuid,
                                          const char *rx_uuid);
void miband_hci_gatt_set_write_callback(MiBandHciGatt *gatt, void *opaque,
                                         MiBandHciGattWriteCallback callback);
bool miband_hci_gatt_queue_notification(MiBandHciGatt *gatt,
                                         const uint8_t *value, size_t length);
bool miband_hci_gatt_take_notification(MiBandHciGatt *gatt, uint8_t *response,
                                        unsigned capacity, unsigned *length);

/* Handle an H4 ACL packet carrying an ATT request. Returns true when the
 * packet belongs to the synthetic peer, including write commands that have
 * no response. response_len is zero for those commands. */
bool miband_hci_gatt_acl(MiBandHciGatt *gatt, const uint8_t *packet,
                         unsigned size, uint8_t *response, unsigned capacity,
                         unsigned *response_len);

#endif
