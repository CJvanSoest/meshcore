// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "mc_aes.h"
#include <string.h>

void mc_aes128_init(mc_aes_t* a, const uint8_t key[16]) {
    AES_init_ctx(&a->ctx, key);
}

void mc_aes128_ecb_encrypt(mc_aes_t* a, const uint8_t in[16], uint8_t out[16]) {
    if (out != in) memcpy(out, in, 16);
    AES_ECB_encrypt(&a->ctx, out);  // in-place
}

void mc_aes128_ecb_decrypt(mc_aes_t* a, const uint8_t in[16], uint8_t out[16]) {
    if (out != in) memcpy(out, in, 16);
    AES_ECB_decrypt(&a->ctx, out);
}

void mc_aes128_cbc_encrypt(mc_aes_t* a, const uint8_t iv[16], size_t len, const uint8_t* in, uint8_t* out) {
    AES_ctx_set_iv(&a->ctx, iv);
    if (out != in) memcpy(out, in, len);
    AES_CBC_encrypt_buffer(&a->ctx, out, len);
}

void mc_aes128_cbc_decrypt(mc_aes_t* a, const uint8_t iv[16], size_t len, const uint8_t* in, uint8_t* out) {
    AES_ctx_set_iv(&a->ctx, iv);
    if (out != in) memcpy(out, in, len);
    AES_CBC_decrypt_buffer(&a->ctx, out, len);
}
