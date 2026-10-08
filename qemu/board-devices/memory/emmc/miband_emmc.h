/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef MIBAND_EMMC_H
#define MIBAND_EMMC_H
#include "hw/qdev-core.h"

/* Attach a soldered eMMC card to a board-selected SD/MMC bus. */
void miband_emmc_attach(BusState *bus, const char *drive_name, Error **errp);
#endif
