/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/sd/sd.h"
#include "sysemu/block-backend.h"
#include "miband_emmc.h"

void miband_emmc_attach(BusState *bus, const char *drive_name, Error **errp)
{
    BlockBackend *backend = blk_by_name(drive_name);
    DeviceState *card;

    if (!backend) {
        error_setg(errp, "eMMC drive backend '%s' does not exist", drive_name);
        return;
    }
    card = qdev_new(TYPE_EMMC);
    if (!qdev_prop_set_drive_err(card, "drive", backend, errp)) {
        object_unref(OBJECT(card));
        return;
    }
    /* With no boot partitions the backend starts at user-area LBA0. */
    qdev_realize_and_unref(card, bus, errp);
}
