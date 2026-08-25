// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "mc_hmac.h"
#include <string.h>
#include "mbedtls/md.h"

// RFC 2104 HMAC over SHA-256. Block size B = 64.
void mc_hmac_sha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len, uint8_t out[32]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    const size_t             B    = 64;

    uint8_t k0[64] = {0};  // key hashed if longer than the block, else zero-padded
    if (key_len > B)
        mbedtls_md(info, key, key_len, k0);
    else
        memcpy(k0, key, key_len);

    uint8_t ipad[64], opad[64];
    for (size_t i = 0; i < B; i++) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, info, 0);

    uint8_t inner[32];
    mbedtls_md_starts(&ctx);  // inner = H(ipad || msg)
    mbedtls_md_update(&ctx, ipad, B);
    mbedtls_md_update(&ctx, msg, msg_len);
    mbedtls_md_finish(&ctx, inner);

    mbedtls_md_starts(&ctx);  // out = H(opad || inner)
    mbedtls_md_update(&ctx, opad, B);
    mbedtls_md_update(&ctx, inner, 32);
    mbedtls_md_finish(&ctx, out);

    mbedtls_md_free(&ctx);
}
