/* Generic offline HCI controller; intentionally no radio/link simulation.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/guest-random.h"
#include "crypto/cipher.h"
#include "qapi/error.h"
#include "miband_hci_synthetic.h"

struct MiBandHciSynthetic {
    uint8_t local_name[248];
    uint8_t adv_data[31], scan_rsp_data[31];
    bool le_adv_enabled, le_scan_enabled;
};

MiBandHciSynthetic *miband_hci_synthetic_new(const char *local_name)
{
    MiBandHciSynthetic *synthetic = g_new0(MiBandHciSynthetic, 1);
    memcpy(synthetic->local_name, local_name,
           MIN(strlen(local_name), sizeof(synthetic->local_name)));
    return synthetic;
}

void miband_hci_synthetic_free(MiBandHciSynthetic *synthetic)
{
    g_free(synthetic);
}

bool miband_hci_synthetic_expected_length(uint16_t opcode, unsigned length)
{
    switch (opcode) {
    case 0x0c01:
        return length == 8;
    case 0x0c03:
        return length == 0;
    case 0x0c05:
        /* Event-filter entries are controller/product specific. */
        return true;
    case 0x0c13:
        return length == 248;
    case 0x0c14:
        return length == 0;
    case 0x0c31:
        return length == 1;
    case 0x0c33:
        return length == 7;
    case 0x0c35:
        /* Each entry is a 16-bit connection handle and 16-bit count. */
        return length >= 1 && ((length - 1) % 4) == 0;
    case 0x1001:
    case 0x1002:
    case 0x1003:
    case 0x1005:
    case 0x1009:
        return length == 0;
    case 0x1004:
        return length == 1;
    case 0x2001:
        return length == 8;
    case 0x2002:
    case 0x2003:
    case 0x2007:
    case 0x200e:
    case 0x200f:
    case 0x2010:
    case 0x2018:
    case 0x201c:
    case 0x201f:
        return length == 0;
    case 0x2005:
        return length == 6;
    case 0x2006:
        return length == 15;
    case 0x2008:
    case 0x2009:
        return length == 32;
    case 0x200a:
        return length == 1;
    case 0x200b:
        return length == 7;
    case 0x200c:
        return length == 2;
    case 0x200d:
        return length == 25;
    case 0x0406:
        return length == 3;
    case 0x2011:
    case 0x2012:
        return length == 7;
    case 0x2013:
        return length == 14;
    case 0x2014:
        return length == 5;
    case 0x2015:
        return length == 2;
    case 0x2016:
        return length == 2;
    case 0x2017:
        return length == 32;
    case 0x2019:
        return length == 28;
    case 0x201a:
        return length == 18;
    case 0x201b:
        return length == 2;
    case 0x201d:
        return length == 1;
    case 0x201e:
        return length == 3;
    default:
        /* Unknown standard commands are rejected by the command handler. */
        return true;
    }
}

/* HCI LE Encrypt takes a 128-bit key followed by a 128-bit plaintext and
 * returns the AES-128-ECB ciphertext after the status byte.  Hosts use
 * this primitive for legacy SMP confirmation/STK calculations. */
static bool synthetic_le_encrypt(const uint8_t *command, uint8_t *result)
{
    Error *error = NULL;
    QCryptoCipher *cipher;
    int ret;

    cipher = qcrypto_cipher_new(QCRYPTO_CIPHER_ALG_AES_128,
                                QCRYPTO_CIPHER_MODE_ECB,
                                command + 4, 16, &error);
    if (!cipher) {
        error_report_err(error);
        return false;
    }
    ret = qcrypto_cipher_encrypt(cipher, command + 20, result, 16, &error);
    qcrypto_cipher_free(cipher);
    if (ret < 0) {
        error_report_err(error);
        return false;
    }
    return true;
}

bool miband_hci_synthetic_command(MiBandHciSynthetic *synthetic,
    const MiBandHciSyntheticIdentity *identity, const uint8_t *data,
    unsigned size, MiBandHciSyntheticResult *result)
{
    uint8_t *params = result->params;
    unsigned params_len = 0;
    uint8_t status = 0;
    uint16_t opcode;
    bool known = true;

    memset(result, 0, sizeof(*result));
    if (size < 4 || data[0] != 0x01 || data[3] != size - 4) {
        result->status = 0x12;
        return true;
    }
    opcode = data[1] | (data[2] << 8);
    if (!miband_hci_synthetic_expected_length(opcode, size - 4)) {
        result->status = 0x12;
        return true;
    }
    if (size > 4) {
        params_len = MIN(size - 4, sizeof(result->params));
        memcpy(params, data + 4, params_len);
    }
    switch (opcode) {
    case 0x0c01:
        /* The event mask is host policy.  The synthetic controller keeps
         * accepting events and returns a normal status-only completion. */
        params_len = 0;
        break;
    case 0x0c05:
        params_len = 0;
        break;
    case 0x0c31:
    case 0x0c33:
    case 0x0c35:
        params_len = 0;
        break;
    case 0x0c03:
        synthetic->le_adv_enabled = false;
        synthetic->le_scan_enabled = false;
        params_len = 0;
        break;
    case 0x0c13:
        memcpy(synthetic->local_name, data + 4, sizeof(synthetic->local_name));
        params_len = 0;
        break;
    case 0x0c14:
        memcpy(params, synthetic->local_name, sizeof(synthetic->local_name));
        params_len = sizeof(synthetic->local_name);
        break;
    case 0x1001:
        /* Bluetooth 4.0 is the first HCI/LMP version with LE. Version
         * 5 means Bluetooth 3.0 and contradicts this controller's LE
         * feature bit and implemented legacy LE command family. */
        params[0] = 6;
        params[1] = 1;
        params[2] = 0;
        params[3] = 6;
        params[4] = 0xbf;
        params[5] = 0;
        params[6] = 1;
        params[7] = 0;
        params_len = 8;
        break;
    case 0x1002:
        memset(params, 0, 64);
        /* Bluetooth Supported Commands octets. Report the implemented
         * discovery and legacy LE setup subset rather than disabling every
         * follow-up probe. Do not advertise extended advertising or
         * PHY/DLE/SMP operations whose controller procedures are not modeled. */
        params[5] = 0xc0;   /* Set Event Mask, Reset */
        params[14] = 0xe8;  /* Version, features, extended features, buffers */
        params[15] = 0x02;  /* Read BD_ADDR */
        params[25] = 0xf7;  /* LE mask/buffer/features, address, advertising */
        params[26] = 0x0f;  /* Scan response, advertising/scan enable/setup */
        params[27] = 0xc0;  /* LE Encrypt, LE Rand */
        params[28] = 0x08;  /* LE Read Supported States */
        params_len = 64;
        break;
    case 0x1003:
        memcpy(params, identity->features, 8);
        params_len = 8;
        break;
    case 0x1004:
        params[0] = data[4];
        params[1] = 0;
        memcpy(params + 2, identity->extended_features,
               8);
        params_len = 10;
        break;
    case 0x1005:
        /* Read Buffer Size: ACL length, SCO length, ACL count, SCO count. */
        params[0] = 0xfb;
        params[1] = 0x00;
        params[2] = 0x00;
        params[3] = 0x0a;
        params[4] = 0x00;
        params[5] = 0x00;
        params[6] = 0x00;
        params_len = 7;
        break;
    case 0x1009:
        memcpy(params, identity->address, 6);
        params_len = 6;
        break;
    case 0x2001:
        params_len = 0;
        break;
    case 0x2002:
        /* LE Read Buffer Size: 251-byte ACL packets, ten buffers. */
        params[0] = 0xfb;
        params[1] = 0x00;
        params[2] = 0x0a;
        params_len = 3;
        break;
    case 0x2003:
        /* LE features are not LMP features (FC81/1003). The synthetic
         * backend models no optional link-layer procedures. A real
         * controller returns its own independent feature bits. */
        memset(params, 0, 8);
        params_len = 8;
        break;
    case 0x2005:
        params_len = 0;
        break;
    case 0x2006:
        params_len = 0;
        break;
    case 0x2007:
        params[0] = 0;
        params_len = 1;
        break;
    case 0x2008:
        memcpy(synthetic->adv_data, data + 5, sizeof(synthetic->adv_data));
        params_len = 0;
        break;
    case 0x2009:
        memcpy(synthetic->scan_rsp_data, data + 5, sizeof(synthetic->scan_rsp_data));
        params_len = 0;
        break;
    case 0x200a:
        synthetic->le_adv_enabled = data[4] != 0;
        params_len = 0;
        break;
    case 0x200b:
        params_len = 0;
        break;
    case 0x200c:
        synthetic->le_scan_enabled = data[4] != 0;
        params_len = 0;
        break;
    case 0x200d:
        params_len = 0;
        break;
    case 0x200f:
        params[0] = 16;
        params_len = 1;
        break;
    case 0x2010:
    case 0x2011:
    case 0x2012:
    case 0x2013:
    case 0x2014:
    case 0x2015:
    case 0x2016:
    case 0x2019:
    case 0x201a:
    case 0x201b:
    case 0x201d:
    case 0x201e:
        /* These commands have no useful synthetic side effect yet, but
         * their standard completion still must be successful so the AP
         * host can advance its state machine. */
        params_len = 0;
        break;
    case 0x2017:
        if (synthetic_le_encrypt(data, params)) {
            params_len = 16;
        } else {
            status = 0x01;
            params_len = 0;
        }
        break;
    case 0x201c:
        /* LE Supported States, conservatively advertise the common
         * scanner/advertiser/central/peripheral combinations. */
        memcpy(params, (const uint8_t[]){
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03
        }, 8);
        params_len = 8;
        break;
    case 0x2018:
        qemu_guest_getrandom_nofail(params, 8);
        params_len = 8;
        break;
    case 0x201f:
        params[0] = 0;
        params_len = 1;
        break;
    case 0x0406:
        /* Disconnect completion is handled by the controller adapter when a
         * synthetic connection is active. The command itself is accepted so
         * the AP can close a peer without a physical HCI backend. */
        params_len = 0;
        break;
    default:
        status = 0x01;
        known = false;
        break;
    }
    result->status = status;
    result->params_len = params_len;
    return known;
}

unsigned miband_hci_synthetic_connection_complete(const uint8_t *command,
                                                  unsigned size,
                                                  uint16_t handle,
                                                  uint8_t *event)
{
    uint8_t peer_type;

    if (!command || !event || size != 29 || command[0] != 0x01 ||
        command[1] != 0x0d || command[2] != 0x20 || command[3] != 25) {
        return 0;
    }
    /* LE Create Connection: filter policy, peer address type and address. */
    peer_type = command[9];
    event[0] = 0x04;
    event[1] = 0x3e;
    event[2] = 0x13;
    event[3] = 0x01;             /* LE Connection Complete */
    event[4] = 0x00;             /* status */
    event[5] = handle;
    event[6] = handle >> 8;
    event[7] = 0x00;             /* central role */
    event[8] = peer_type;
    memcpy(event + 9, command + 10, 6);
    event[15] = 0x18;             /* 30 ms interval */
    event[16] = 0x00;
    event[17] = 0x00;             /* peripheral latency */
    event[18] = 0x00;
    event[19] = 0x2a;             /* 4.2 s supervision timeout */
    event[20] = 0x01;
    event[21] = 0x00;             /* clock accuracy */
    return 22;
}

unsigned miband_hci_synthetic_acl(const uint8_t *data, unsigned size,
                                  uint8_t *event)
{
    static const uint8_t completed[] = {
        0x04, 0x13, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00
    };
    if (!size || data[0] != 0x02) {
        return 0;
    }
    memcpy(event, completed, sizeof(completed));
    return sizeof(completed);
}
