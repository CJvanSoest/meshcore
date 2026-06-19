// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#include "mc_crypto.h"

#include <stdio.h>
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

uint16_t mc_crypto_region_transport_code(const char *region_name, uint8_t type,
                                         const uint8_t *payload, uint16_t payload_len) {
    // Upstream MeshCore RegionMap::getTransportKeysFor prepends '#' to the
    // region name before SHA256-deriving the transport key. Match that exactly
    // or scope-aware relays compute a different code and drop us.
    char scope_name[35];
    if (region_name[0] == '#') snprintf(scope_name, sizeof(scope_name), "%s", region_name);
    else                       snprintf(scope_name, sizeof(scope_name), "#%s", region_name);

    uint8_t region_key[16];
    {
        uint8_t digest[32];
        mbedtls_sha256((const uint8_t *)scope_name, strlen(scope_name), digest, 0);
        memcpy(region_key, digest, sizeof(region_key));
    }

    uint8_t mac[32];
    {
        mbedtls_md_context_t md;
        mbedtls_md_init(&md);
        mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
        mbedtls_md_hmac_starts(&md, region_key, sizeof(region_key));
        mbedtls_md_hmac_update(&md, &type, 1);
        mbedtls_md_hmac_update(&md, payload, payload_len);
        mbedtls_md_hmac_finish(&md, mac);
        mbedtls_md_free(&md);
    }

    uint16_t code;
    memcpy(&code, mac, 2);
    if (code == 0x0000)      code = 0x0001;  // 0 / 0xFFFF are reserved sentinels
    else if (code == 0xFFFF) code = 0xFFFE;
    return code;
}
