// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Ilias el Matani authored this file and claims no copyright or other rights
// in it, contributing it freely for anyone to use and share.

#include "mc_crypto.h"

#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

bool mc_crypto_grp_decrypt(meshcore_grp_txt_t *grp, const uint8_t *key) {
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    key, MESHCORE_CIPHER_KEY_SIZE,
                    grp->data, grp->data_length,
                    mac);
    if (memcmp(mac, grp->mac, MESHCORE_CIPHER_MAC_SIZE) != 0) return false;

    grp->decrypted.data_length = grp->data_length;
    memcpy(grp->decrypted.data, grp->data, grp->data_length);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, key, 128);
    for (int i = 0; i < grp->decrypted.data_length / MESHCORE_CIPHER_BLOCK_SIZE; i++) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT,
                               &grp->decrypted.data[i * MESHCORE_CIPHER_BLOCK_SIZE],
                               &grp->decrypted.data[i * MESHCORE_CIPHER_BLOCK_SIZE]);
    }
    mbedtls_aes_free(&aes);

    // Parse: timestamp(4) | text_type(1) | text
    if (grp->decrypted.data_length < 5) return false;
    memcpy(&grp->decrypted.timestamp, grp->decrypted.data, 4);
    grp->decrypted.text_type = grp->decrypted.data[4];
    grp->decrypted.data[grp->decrypted.data_length - 1] = '\0';
    grp->decrypted.text = (char *)&grp->decrypted.data[5];
    return true;
}

void mc_crypto_grp_encrypt(const uint8_t *key, const uint8_t *plain,
                           size_t padded_len, uint8_t *out_cipher,
                           uint8_t out_mac[32]) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    for (size_t i = 0; i < padded_len / MESHCORE_CIPHER_BLOCK_SIZE; i++) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT,
                               &plain[i * MESHCORE_CIPHER_BLOCK_SIZE],
                               &out_cipher[i * MESHCORE_CIPHER_BLOCK_SIZE]);
    }
    mbedtls_aes_free(&aes);

    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    key, MESHCORE_CIPHER_KEY_SIZE,
                    out_cipher, (uint16_t)padded_len, out_mac);
}

void mc_crypto_ack_crc(const uint8_t head5[5], const char *text, size_t text_len,
                       const uint8_t pubkey[MESHCORE_PUB_KEY_SIZE],
                       uint8_t out_crc[4]) {
    uint8_t sha_out[32];
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);
    mbedtls_sha256_update(&sha_ctx, head5, 5);
    mbedtls_sha256_update(&sha_ctx, (const uint8_t *)text, text_len);
    mbedtls_sha256_update(&sha_ctx, pubkey, MESHCORE_PUB_KEY_SIZE);
    mbedtls_sha256_finish(&sha_ctx, sha_out);
    mbedtls_sha256_free(&sha_ctx);
    memcpy(out_crc, sha_out, 4);
}
