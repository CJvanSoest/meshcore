// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Direct-message (TXT_MSG) crypto, split out of mc_crypto.c because it needs
// the ed25519 ECDH from vendor (the channel paths are mbedtls only). Host
// tested in tests/test_mc_crypto_dm.c with an esp_random stub.

#include <string.h>
#include "ed25519.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mc_crypto.h"

void mc_crypto_dm_encrypt(const uint8_t target_pub[MESHCORE_PUB_KEY_SIZE], const uint8_t* my_prv, const uint8_t* plain,
                          size_t padded_len, uint8_t* out_cipher, uint8_t out_mac[32]) {
    uint8_t shared[32];
    ed25519_key_exchange(shared, target_pub, my_prv);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, shared, 128);
    for (size_t i = 0; i < padded_len / 16; i++)
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, &plain[i * 16], &out_cipher[i * 16]);
    mbedtls_aes_free(&aes);

    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), shared, 32, out_cipher, padded_len, out_mac);
}

bool mc_crypto_dm_decrypt(const uint8_t* payload, uint8_t payload_len, const uint8_t sender_pub[MESHCORE_PUB_KEY_SIZE],
                          const uint8_t* my_prv, uint8_t* out_plaintext, int* out_text_len,
                          uint8_t out_good_secret[32]) {
    const uint8_t* mac_ct     = &payload[2];
    int            mac_ct_len = payload_len - 2;
    if (mac_ct_len < MESHCORE_CIPHER_MAC_SIZE + 16) return false;

    const uint8_t* ciphertext = mac_ct + MESHCORE_CIPHER_MAC_SIZE;
    int            ct_len     = mac_ct_len - MESHCORE_CIPHER_MAC_SIZE;

    // Four candidate secrets: the ed25519->x25519 converted vs raw scalar mul,
    // each tried as a 32- and 16-byte HMAC key. Upstream peers have keyed all
    // four ways across versions; accept on the first whose HMAC[0..1] matches.
    uint8_t secret[32], secret_raw[32];
    ed25519_key_exchange(secret, sender_pub, my_prv);
    ed25519_key_exchange_raw(secret_raw, sender_pub, my_prv);

    uint8_t hmac_conv[32], hmac_raw[32], hmac_conv16[32], hmac_raw16[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), secret, 32, ciphertext, ct_len, hmac_conv);
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), secret_raw, 32, ciphertext, ct_len, hmac_raw);
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), secret, 16, ciphertext, ct_len, hmac_conv16);
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), secret_raw, 16, ciphertext, ct_len, hmac_raw16);

    uint8_t        exp0 = mac_ct[0], exp1 = mac_ct[1];
    const uint8_t* good = NULL;
    if (hmac_conv[0] == exp0 && hmac_conv[1] == exp1)
        good = secret;
    else if (hmac_raw[0] == exp0 && hmac_raw[1] == exp1)
        good = secret_raw;
    else if (hmac_conv16[0] == exp0 && hmac_conv16[1] == exp1)
        good = secret;
    else if (hmac_raw16[0] == exp0 && hmac_raw16[1] == exp1)
        good = secret_raw;
    if (!good) return false;

    mbedtls_aes_context aes_ctx;
    mbedtls_aes_init(&aes_ctx);
    mbedtls_aes_setkey_dec(&aes_ctx, good, 128);
    for (int bi = 0; bi + 16 <= ct_len; bi += 16)
        mbedtls_aes_crypt_ecb(&aes_ctx, MBEDTLS_AES_DECRYPT, ciphertext + bi, out_plaintext + bi);
    mbedtls_aes_free(&aes_ctx);

    *out_text_len = ct_len - 5;
    memcpy(out_good_secret, good, 32);
    return true;
}
