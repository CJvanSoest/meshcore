// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "cert_gen.h"
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"

static const char* TAG = "cert-gen";

static const char* NS     = "http_cert";
static const char* K_CERT = "cert_pem";
static const char* K_KEY  = "key_pem";

esp_err_t cert_gen_nvs_load(char* cert_pem, size_t cert_cap, char* key_pem, size_t key_cap) {
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t cl = cert_cap, kl = key_cap;
    err = nvs_get_str(h, K_CERT, cert_pem, &cl);
    if (err == ESP_OK) err = nvs_get_str(h, K_KEY, key_pem, &kl);
    nvs_close(h);
    return err;
}

esp_err_t cert_gen_nvs_save(const char* cert_pem, const char* key_pem) {
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, K_CERT, cert_pem);
    if (err == ESP_OK) err = nvs_set_str(h, K_KEY, key_pem);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t cert_gen_nvs_clear(void) {
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, K_CERT);
    nvs_erase_key(h, K_KEY);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Reseed callback for mbedtls_ctr_drbg from ESP hardware RNG. The
// CTR_DRBG context still does its own bookkeeping; this just hands it a
// fresh seed when it asks. esp_fill_random doesn't fail.
static int esp_rng_cb(void* ctx, unsigned char* buf, size_t len) {
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

esp_err_t cert_gen_self_signed(char* cert_pem, size_t cert_cap, char* key_pem, size_t key_cap) {
    if (!cert_pem || !key_pem || cert_cap < 256 || key_cap < 128) {
        return ESP_ERR_INVALID_ARG;
    }
    int                      rc     = 0;
    esp_err_t                result = ESP_OK;
    mbedtls_pk_context       pk;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  entropy;
    mbedtls_x509write_cert   crt;

    mbedtls_pk_init(&pk);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&entropy);
    mbedtls_x509write_crt_init(&crt);

    // Seed the DRBG from the ESP hardware RNG. Using a custom rng_cb keeps
    // us independent of MBEDTLS_ENTROPY_HARDWARE_ALT kconfig wiring.
    static const char* PERS = "meshcore-cert";
    rc                      = mbedtls_ctr_drbg_seed(&drbg, esp_rng_cb, NULL, (const unsigned char*)PERS, strlen(PERS));
    if (rc != 0) {
        ESP_LOGE(TAG, "drbg seed: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }

    // ECDSA P-256 key. ~1-2 s on ESP32-P4 the first time.
    rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (rc != 0) {
        ESP_LOGE(TAG, "pk_setup: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }
    rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &drbg);
    if (rc != 0) {
        ESP_LOGE(TAG, "ecp_gen_key: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }

    // Self-signed: issuer == subject. Browsers / iOS Profile install care
    // about CN matching the hostname they connect to, so CN=tanmatsu.local.
    static const char* SUBJ = "CN=tanmatsu.local,O=MeshCore on Tanmatsu";
    rc                      = mbedtls_x509write_crt_set_subject_name(&crt, SUBJ);
    if (rc != 0) {
        ESP_LOGE(TAG, "set_subject: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }
    rc = mbedtls_x509write_crt_set_issuer_name(&crt, SUBJ);
    if (rc != 0) {
        ESP_LOGE(TAG, "set_issuer: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }

    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);

    // Random 16-byte serial. mbedTLS deprecated the MPI-taking set_serial;
    // set_serial_raw takes the big-endian bytes directly. High bit cleared
    // keeps it unambiguously positive per X.509 rules.
    unsigned char serial_bytes[16];
    esp_fill_random(serial_bytes, sizeof(serial_bytes));
    serial_bytes[0] &= 0x7F;
    rc               = mbedtls_x509write_crt_set_serial_raw(&crt, serial_bytes, sizeof(serial_bytes));
    if (rc != 0) {
        ESP_LOGE(TAG, "set_serial_raw: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }

    // 2026-01-01 to 2036-01-01 — well outside any reasonable badge
    // service life. No usable RTC yet at first-boot generation time, so
    // a hardcoded epoch window beats e.g. NTP-derived dates.
    rc = mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20360101000000");
    if (rc != 0) {
        ESP_LOGE(TAG, "set_validity: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }

    rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0);  // CA:TRUE
    if (rc != 0) {
        ESP_LOGE(TAG, "set_basic: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }
    rc = mbedtls_x509write_crt_set_key_usage(
        &crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT | MBEDTLS_X509_KU_KEY_CERT_SIGN);
    if (rc != 0) {
        ESP_LOGE(TAG, "set_key_usage: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }

    // SAN extension via the high-level mbedTLS helper. DNS-only SAN, paired
    // with mDNS publishing of "tanmatsu.local" — keeps the cert stable
    // across IP / network changes.
    mbedtls_x509_san_list san_local          = {0};
    mbedtls_x509_san_list san_short          = {0};
    san_local.node.type                      = MBEDTLS_X509_SAN_DNS_NAME;
    san_local.node.san.unstructured_name.p   = (unsigned char*)"tanmatsu.local";
    san_local.node.san.unstructured_name.len = strlen("tanmatsu.local");
    san_local.next                           = &san_short;
    san_short.node.type                      = MBEDTLS_X509_SAN_DNS_NAME;
    san_short.node.san.unstructured_name.p   = (unsigned char*)"tanmatsu";
    san_short.node.san.unstructured_name.len = strlen("tanmatsu");
    san_short.next                           = NULL;
    rc                                       = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san_local);
    if (rc != 0) {
        ESP_LOGE(TAG, "set_san: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }

    rc = mbedtls_x509write_crt_pem(&crt, (unsigned char*)cert_pem, cert_cap, mbedtls_ctr_drbg_random, &drbg);
    if (rc != 0) {
        ESP_LOGE(TAG, "crt_pem: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }

    rc = mbedtls_pk_write_key_pem(&pk, (unsigned char*)key_pem, key_cap);
    if (rc != 0) {
        ESP_LOGE(TAG, "pk_pem: -0x%04x", -rc);
        result = ESP_FAIL;
        goto out;
    }

    ESP_LOGI(TAG, "generated ECDSA P-256 self-signed cert (cert=%u B, key=%u B)", (unsigned)strlen(cert_pem),
             (unsigned)strlen(key_pem));

out:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    return result;
}

esp_err_t cert_gen_load_or_create(char* cert_pem, size_t cert_cap, char* key_pem, size_t key_cap) {
    esp_err_t err = cert_gen_nvs_load(cert_pem, cert_cap, key_pem, key_cap);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "loaded existing cert from NVS");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "no NVS cert (err=0x%x), generating fresh", err);
    err = cert_gen_self_signed(cert_pem, cert_cap, key_pem, key_cap);
    if (err != ESP_OK) return err;
    esp_err_t save_err = cert_gen_nvs_save(cert_pem, key_pem);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "cert generated but NVS save failed: 0x%x — will regen next boot", save_err);
    }
    return ESP_OK;
}

esp_err_t cert_gen_fingerprint_hex(const char* cert_pem, char* hex_out, size_t hex_cap) {
    if (!cert_pem || !hex_out || hex_cap < 65) return ESP_ERR_INVALID_ARG;

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    int rc = mbedtls_x509_crt_parse(&crt, (const unsigned char*)cert_pem, strlen(cert_pem) + 1);
    if (rc != 0) {
        mbedtls_x509_crt_free(&crt);
        return ESP_FAIL;
    }

    unsigned char digest[32];
    rc = mbedtls_sha256(crt.raw.p, crt.raw.len, digest, 0);
    mbedtls_x509_crt_free(&crt);
    if (rc != 0) return ESP_FAIL;

    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_out[i * 2]     = HEX[digest[i] >> 4];
        hex_out[i * 2 + 1] = HEX[digest[i] & 0x0F];
    }
    hex_out[64] = '\0';
    return ESP_OK;
}
