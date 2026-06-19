// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#include "advert_sign.h"

#include <string.h>

#include "meshcore/packet.h"

uint8_t meshcore_advert_signable_bytes(const uint8_t *payload,
                                       uint8_t payload_len, uint8_t *out) {
    const uint8_t head = MESHCORE_PUB_KEY_SIZE + 4;  // pub_key + timestamp
    if (payload_len < head) return 0;

    memcpy(out, payload, head);
    uint8_t out_len = head;

    const uint8_t after_sig = head + MESHCORE_SIGNATURE_SIZE;
    if (payload_len > after_sig) {
        memcpy(out + out_len, payload + after_sig, payload_len - after_sig);
        out_len += payload_len - after_sig;
    }
    return out_len;
}
