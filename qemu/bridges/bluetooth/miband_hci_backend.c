/* Host HCI backend configuration.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "miband_hci_backend.h"

static bool backend_number(const char *text, unsigned maximum, unsigned *out)
{
    char *end;
    unsigned long number;

    if (!g_ascii_isdigit(text[0])) {
        return false;
    }
    errno = 0;
    number = strtoul(text, &end, 0);
    if (errno || end == text || *end || number > maximum) {
        return false;
    }
    *out = number;
    return true;
}

bool miband_hci_backend_parse(const char *spec, MiBandHciBackendConfig *config,
                              Error **errp)
{
    char *separator;

    *config = (MiBandHciBackendConfig) { .kind = MIBAND_HCI_SYNTHETIC };
    if (!spec || !strcmp(spec, "synthetic") || !strcmp(spec, "none")) {
        return true;
    }
    if (g_str_has_prefix(spec, "serial:") ||
        g_str_has_prefix(spec, "serial-btdm:")) {
        config->kind = MIBAND_HCI_SERIAL;
        config->dual_mode = g_str_has_prefix(spec, "serial-btdm:");
        config->path = g_strdup(strchr(spec, ':') + 1);
        config->baud = 115200;
        separator = strrchr(config->path, '@');
        if (!separator) {
            separator = strrchr(config->path, ',');
        }
        if (separator) {
            *separator++ = '\0';
            if (!backend_number(separator, UINT_MAX, &config->baud) ||
                !config->baud) {
                error_setg(errp, "miband BT serial invalid baud=%s", separator);
                return false;
            }
        }
        if (!config->path[0]) {
            error_setg(errp, "miband BT serial path is empty");
            return false;
        }
        return true;
    }
    if (!strcmp(spec, "host-hci") || g_str_has_prefix(spec, "host-hci:")) {
        config->kind = MIBAND_HCI_LINUX_RAW;
        if (spec[8] && !backend_number(spec + 9, 0xffff, &config->device)) {
            error_setg(errp, "miband BT invalid host HCI device=%s", spec + 9);
            return false;
        }
        return true;
    }
    error_setg(errp, "miband BT unknown backend=%s", spec);
    return false;
}

void miband_hci_backend_clear(MiBandHciBackendConfig *config)
{
    g_clear_pointer(&config->path, g_free);
}
