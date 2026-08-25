// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>

#include "mc_crypto.h"
#include <stdio.h>
#include <string.h>
#include "mbedtls/md.h"  // HMAC + SHA-256 via the still-public MD API (mbedtls 4.1)
#include "mc_aes.h"      // vendored AES-128 (mbedtls/aes.h is private in mbedtls 4.1)

bool mc_crypto_grp_decrypt(meshcore_grp_txt_t* grp, const uint8_t* key) {
    uint8_t mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, MESHCORE_CIPHER_KEY_SIZE, grp->data,
                    grp->data_length, mac);
    if (memcmp(mac, grp->mac, MESHCORE_CIPHER_MAC_SIZE) != 0) return false;

    grp->decrypted.data_length = grp->data_length;
    memcpy(grp->decrypted.data, grp->data, grp->data_length);
    mc_aes_t aes;
    mc_aes128_init(&aes, key);
    for (int i = 0; i < grp->decrypted.data_length / MESHCORE_CIPHER_BLOCK_SIZE; i++) {
        mc_aes128_ecb_decrypt(&aes, &grp->decrypted.data[i * MESHCORE_CIPHER_BLOCK_SIZE],
                              &grp->decrypted.data[i * MESHCORE_CIPHER_BLOCK_SIZE]);
    }

    // Parse: timestamp(4) | text_type(1) | text
    if (grp->decrypted.data_length < 5) return false;
    memcpy(&grp->decrypted.timestamp, grp->decrypted.data, 4);
    grp->decrypted.text_type                            = grp->decrypted.data[4];
    grp->decrypted.data[grp->decrypted.data_length - 1] = '\0';
    grp->decrypted.text                                 = (char*)&grp->decrypted.data[5];
    return true;
}

void mc_crypto_grp_encrypt(const uint8_t* key, const uint8_t* plain, size_t padded_len, uint8_t* out_cipher,
                           uint8_t out_mac[32]) {
    mc_aes_t aes;
    mc_aes128_init(&aes, key);
    for (size_t i = 0; i < padded_len / MESHCORE_CIPHER_BLOCK_SIZE; i++) {
        mc_aes128_ecb_encrypt(&aes, &plain[i * MESHCORE_CIPHER_BLOCK_SIZE],
                              &out_cipher[i * MESHCORE_CIPHER_BLOCK_SIZE]);
    }

    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, MESHCORE_CIPHER_KEY_SIZE, out_cipher,
                    (uint16_t)padded_len, out_mac);
}

void mc_crypto_ack_crc(const uint8_t head5[5], const char* text, size_t text_len,
                       const uint8_t pubkey[MESHCORE_PUB_KEY_SIZE], uint8_t out_crc[4]) {
    uint8_t              sha_out[32];
    mbedtls_md_context_t sha_ctx;
    mbedtls_md_init(&sha_ctx);
    mbedtls_md_setup(&sha_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
    mbedtls_md_starts(&sha_ctx);
    mbedtls_md_update(&sha_ctx, head5, 5);
    mbedtls_md_update(&sha_ctx, (const uint8_t*)text, text_len);
    mbedtls_md_update(&sha_ctx, pubkey, MESHCORE_PUB_KEY_SIZE);
    mbedtls_md_finish(&sha_ctx, sha_out);
    mbedtls_md_free(&sha_ctx);
    memcpy(out_crc, sha_out, 4);
}

uint16_t mc_crypto_region_transport_code(const char* region_name, uint8_t type, const uint8_t* payload,
                                         uint16_t payload_len) {
    // Upstream MeshCore RegionMap::getTransportKeysFor prepends '#' to the
    // region name before SHA256-deriving the transport key. Match that exactly
    // or scope-aware relays compute a different code and drop us.
    char scope_name[35];
    if (region_name[0] == '#')
        snprintf(scope_name, sizeof(scope_name), "%s", region_name);
    else
        snprintf(scope_name, sizeof(scope_name), "#%s", region_name);

    uint8_t region_key[16];
    {
        uint8_t digest[32];
        mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)scope_name, strlen(scope_name),
                   digest);
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
    if (code == 0x0000)
        code = 0x0001;  // 0 / 0xFFFF are reserved sentinels
    else if (code == 0xFFFF)
        code = 0xFFFE;
    return code;
}
