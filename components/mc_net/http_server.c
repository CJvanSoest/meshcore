// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "http_server.h"

#include "cJSON.h"
#include "cert_gen.h"
#include "esp_event.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_netif_types.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"
#include "settings_nvs.h"
#include "wifi_connection.h"

// On-device generated self-signed cert + key (see cert_gen.c). PEM-text
// blobs live in NVS and survive reflash. iOS clients install the cert as
// a profile once; mDNS publishing means the SAN can stay DNS-only
// (tanmatsu.local) and never needs IP regeneration.
static char s_cert_pem[CERT_GEN_PEM_CERT_MAX];
static char s_key_pem[CERT_GEN_PEM_KEY_MAX];
static bool s_cert_ready  = false;
static bool s_mdns_inited = false;

static const char *TAG = "http-srv";

static httpd_handle_t s_server = NULL;

// ── /test GET ─────────────────────────────────────────────────────────────
// Echoes back the source IP + a small status string. Lets the user verify
// the WiFi+HTTP path end-to-end with a single curl from the same network:
//   curl http://<tanmatsu-ip>/test
static esp_err_t test_get_handler(httpd_req_t *req) {
    (void)req;
    // Static body -- avoids -Werror=format-truncation grumbling about the
    // 512-byte upper bound on req->uri vs a stack buffer, and we don't
    // actually need the path echoed for the round-trip check.
    static const char body[] = "MeshCore-on-Tanmatsu HTTP up.\n";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t s_uri_test = {
    .uri      = "/test",
    .method   = HTTP_GET,
    .handler  = test_get_handler,
    .user_ctx = NULL,
};

// ── GET / ─────────────────────────────────────────────────────────────────
// Friendly index so a user opening https://tanmatsu.local:8443/ in a
// browser doesn't get the bare "Specified method is invalid" error from
// esp_http_server's default 404. Lists the endpoints + suggested commands.
static esp_err_t root_get_handler(httpd_req_t *req) {
    (void)req;
    static const char body[] =
        "MeshCore on Tanmatsu\n"
        "\n"
        "Endpoints:\n"
        "  GET  /test  -- liveness check\n"
        "  GET  /cert  -- download self-signed cert for iOS profile install\n"
        "  POST /ping  -- accept GPS push (X-API-Key header or ?key=...)\n"
        "\n"
        "Example:\n"
        "  curl -k https://tanmatsu.local:8443/test\n";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t s_uri_root = {
    .uri      = "/",
    .method   = HTTP_GET,
    .handler  = root_get_handler,
    .user_ctx = NULL,
};

// ── /ping POST ────────────────────────────────────────────────────────────
// Accepts MeshMapper's wardrive batch payload (see app_third_party_api wiki):
//   { "data": [ {"type":"TX|RX|DISC|TRACE", "lat":<deg>, "lon":<deg>,
//                "timestamp":<unix_s>, ...}, ... ] }
// Up to 50 items per batch, ~15-30 s between batches. We take the most
// recent valid entry (highest timestamp) and save its lat/lon into the
// GPS NVS keys with source = GPS_SRC_HTTP, mirroring what the BLE LATLON
// handler does. Returns 200 OK on save, 400 on parse error, 413 on size
// overrun.

// Max body we'll read in one POST. MeshMapper batches up to 50 pings ≈
// 50 * 200 B = 10 KB worst case (each ping is a small JSON object).
#define PING_MAX_BODY (12 * 1024)

static esp_err_t ping_post_handler(httpd_req_t *req) {
    // Shared-secret auth: /ping writes NVS-backed advert position, so an
    // unauthenticated POST would let any LAN peer pin our location on the
    // mesh. Try X-API-Key header first; fall back to ?key=... query param
    // because iOS Shortcuts drops custom headers when body type is JSON
    // (only Content-Type/Host/User-Agent are auto-sent).
    char hdr[80] = {0};
    bool authed = false;
    if (httpd_req_get_hdr_value_str(req, "X-API-Key", hdr, sizeof(hdr)) == ESP_OK &&
        strcmp(hdr, http_api_key) == 0) {
        authed = true;
    }
    if (!authed) {
        size_t qlen = httpd_req_get_url_query_len(req);
        if (qlen > 0 && qlen < 256) {
            char query[256];
            if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
                char qval[80] = {0};
                if (httpd_query_key_value(query, "key", qval, sizeof(qval)) == ESP_OK &&
                    strcmp(qval, http_api_key) == 0) {
                    authed = true;
                }
            }
        }
    }
    if (!authed) {
        ESP_LOGW(TAG, "/ping unauthorized (no valid X-API-Key header or ?key=)");
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "missing or invalid X-API-Key\n");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || req->content_len > PING_MAX_BODY) {
        ESP_LOGW(TAG, "/ping body size %d out of range", req->content_len);
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "body too large");
        return ESP_FAIL;
    }

    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, buf + total, req->content_len - total);
        if (n <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        total += n;
    }
    buf[total] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, total);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "/ping JSON parse failed");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json parse");
        return ESP_FAIL;
    }

    // Accept three payload shapes:
    //   1. MeshMapper batch:    {"data":[{lat,lon,timestamp,...}, ...]}
    //   2. Flat single push:    {lat, lon, timestamp}  (iOS Shortcuts)
    //   3. OwnTracks HTTP mode: {_type:"location", lat, lon, tst, ...}
    // OwnTracks may also POST other _type values (transition/waypoints/cmd);
    // those are acknowledged with an empty JSON array so the app stops
    // retrying, without touching our advert position.
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "_type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "location") != 0) {
        ESP_LOGI(TAG, "/ping ignoring OwnTracks _type=%s", type->valuestring);
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    double  best_lat = 0, best_lon = 0;
    int64_t best_ts  = -1;
    int     valid    = 0;
    cJSON  *data     = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsArray(data)) {
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, data) {
            cJSON *lat = cJSON_GetObjectItemCaseSensitive(entry, "lat");
            cJSON *lon = cJSON_GetObjectItemCaseSensitive(entry, "lon");
            cJSON *ts  = cJSON_GetObjectItemCaseSensitive(entry, "timestamp");
            if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) continue;
            double la = lat->valuedouble, lo = lon->valuedouble;
            if (la < -90.0 || la > 90.0 || lo < -180.0 || lo > 180.0) continue;
            int64_t t = cJSON_IsNumber(ts) ? (int64_t)ts->valuedouble : 0;
            if (t > best_ts) { best_ts = t; best_lat = la; best_lon = lo; }
            valid++;
        }
    } else {
        // iOS Shortcuts JSON body builder strips the decimal point from
        // "Number" fields (51.87... becomes integer 5187...), so users have
        // to mark lat/lon as Text in the Shortcut UI -- accept both shapes.
        cJSON *lat = cJSON_GetObjectItemCaseSensitive(root, "lat");
        cJSON *lon = cJSON_GetObjectItemCaseSensitive(root, "lon");
        // OwnTracks uses "tst" for the unix timestamp; iOS Shortcut "timestamp".
        cJSON *ts  = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
        if (!ts) ts = cJSON_GetObjectItemCaseSensitive(root, "tst");
        double la = 0, lo = 0;
        bool   ok = false;
        if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
            la = lat->valuedouble; lo = lon->valuedouble; ok = true;
        } else if (cJSON_IsString(lat) && cJSON_IsString(lon)) {
            la = atof(lat->valuestring); lo = atof(lon->valuestring); ok = true;
        }
        if (ok && la >= -90.0 && la <= 90.0 && lo >= -180.0 && lo <= 180.0) {
            best_lat = la;
            best_lon = lo;
            if (cJSON_IsNumber(ts))      best_ts = (int64_t)ts->valuedouble;
            else if (cJSON_IsString(ts)) best_ts = (int64_t)atoll(ts->valuestring);
            else                         best_ts = 0;
            valid = 1;
        }
    }

    cJSON_Delete(root);

    if (valid == 0 || best_ts < 0) {
        ESP_LOGW(TAG, "/ping no valid lat/lon in batch");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no valid entries");
        return ESP_FAIL;
    }

    gps_lat_e6         = (int32_t)(best_lat * 1e6);
    gps_lon_e6         = (int32_t)(best_lon * 1e6);
    gps_position_valid = true;
    gps_last_source    = GPS_SRC_HTTP;
    save_gps_coords();
    ESP_LOGI(TAG, "/ping accepted: lat=%.6f lon=%.6f ts=%lld (from %d entries)",
             best_lat, best_lon, (long long)best_ts, valid);

    // Empty JSON array satisfies OwnTracks ("no friends/cmds to merge")
    // and is harmless for curl / iOS Shortcuts callers.
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "[]");
    return ESP_OK;
}

static const httpd_uri_t s_uri_ping = {
    .uri      = "/ping",
    .method   = HTTP_POST,
    .handler  = ping_post_handler,
    .user_ctx = NULL,
};

// ── /cert GET ─────────────────────────────────────────────────────────────
// Serves the server's self-signed certificate so the user can install it
// as a trusted profile on iOS (Settings > General > VPN & Device
// Management > Profile). Without this dance, iOS rejects HTTPS to our
// self-signed endpoint with "untrusted certificate".
static esp_err_t cert_get_handler(httpd_req_t *req) {
    if (!s_cert_ready) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cert not ready");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/x-x509-ca-cert");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"tanmatsu.crt\"");
    httpd_resp_send(req, s_cert_pem, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static const httpd_uri_t s_uri_cert = {
    .uri      = "/cert",
    .method   = HTTP_GET,
    .handler  = cert_get_handler,
    .user_ctx = NULL,
};

// Load (or generate-on-first-boot) the cert+key into the static PEM
// buffers. Idempotent: subsequent calls noop unless s_cert_ready was
// explicitly cleared (e.g. by http_server_regen_cert below).
static void cert_ensure_loaded(void) {
    if (s_cert_ready) return;
    esp_err_t err = cert_gen_load_or_create(s_cert_pem, sizeof(s_cert_pem),
                                            s_key_pem,  sizeof(s_key_pem));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cert load_or_create failed: 0x%x", err);
        return;
    }
    s_cert_ready = true;
}

// One-time mDNS publish. Without this, iOS clients can't resolve
// "tanmatsu.local" and the cert SAN we issued is useless — they'd have
// to type the LAN IP, which won't match the SAN. mdns_init is safe to
// call once; hostname/service registration is idempotent across reconnects.
static void mdns_ensure_started(void) {
    if (s_mdns_inited) return;
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: 0x%x", err);
        return;
    }
    mdns_hostname_set("tanmatsu");
    mdns_instance_name_set("MeshCore on Tanmatsu");
    mdns_service_add(NULL, "_https", "_tcp", 8443, NULL, 0);
    s_mdns_inited = true;
    ESP_LOGI(TAG, "mDNS up: tanmatsu.local + _https._tcp:8443");
}

// ── Server lifecycle ─────────────────────────────────────────────────────
static void server_start(void) {
    if (s_server != NULL) return;
    cert_ensure_loaded();
    if (!s_cert_ready) {
        ESP_LOGE(TAG, "no cert available — skipping HTTPS server start");
        return;
    }
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.servercert     = (const uint8_t *)s_cert_pem;
    cfg.servercert_len = strlen(s_cert_pem) + 1;  // PEM parser wants NUL
    cfg.prvtkey_pem    = (const uint8_t *)s_key_pem;
    cfg.prvtkey_len    = strlen(s_key_pem) + 1;
    cfg.httpd.lru_purge_enable = true;
    // Default 443 is intercepted by some routers (MikroTik captive-portal
    // / SNI inspection). 8443 is the registered alt-HTTPS port and passes
    // through transparently; MeshMapper accepts any port in the URL.
    cfg.port_secure = 8443;
    if (httpd_ssl_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ssl_start failed");
        s_server = NULL;
        return;
    }
    httpd_register_uri_handler(s_server, &s_uri_root);
    httpd_register_uri_handler(s_server, &s_uri_test);
    httpd_register_uri_handler(s_server, &s_uri_ping);
    httpd_register_uri_handler(s_server, &s_uri_cert);
    mdns_ensure_started();
    ESP_LOGI(TAG, "HTTPS server up on :%d (cert=%u B, key=%u B)",
             cfg.port_secure,
             (unsigned)strlen(s_cert_pem),
             (unsigned)strlen(s_key_pem));
}

// Wipe NVS cert + regenerate. Restarts the server so the new key is live
// before we return. Used by the Settings "Regen Cert" action row.
esp_err_t http_server_regen_cert(void) {
    cert_gen_nvs_clear();
    s_cert_ready = false;
    if (s_server != NULL) {
        httpd_ssl_stop(s_server);
        s_server = NULL;
    }
    server_start();
    return s_cert_ready ? ESP_OK : ESP_FAIL;
}

// Read-only accessor for the live cert PEM. UI uses this to compute the
// fingerprint shown in Settings. Returns NULL until first cert load.
const char *http_server_cert_pem(void) {
    return s_cert_ready ? s_cert_pem : NULL;
}

static void server_stop(void) {
    if (s_server == NULL) return;
    httpd_ssl_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "HTTPS server stopped");
}

// Event-driven supervisor: bind the server to IP_EVENT_STA_GOT_IP / lost-ip
// rather than polling wifi_connection_is_connected(). A poll-based version
// missed sub-second disconnect/reconnect cycles (e.g. the launcher
// reattaching after a brief blip), leaving the bound socket dead. Hooking
// the events makes us react to every transition.

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    // Always restart on got_ip -- even when the IP didn't change, the
    // underlying socket may have churned. server_start is idempotent
    // because server_stop has been called in lost_ip path.
    server_stop();
    server_start();
}

static void on_lost_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    server_stop();
}

static void on_sta_disconnect(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    // Disconnect without a lost_ip first -- still stop so we don't leak
    // a dead socket while waiting for the wifi-manager to reassociate.
    server_stop();
}

void http_server_supervisor_start(void) {
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP,     on_got_ip,         NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_LOST_IP,    on_lost_ip,        NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_sta_disconnect, NULL);
    // If we boot when the launcher already brought WiFi up, IP_EVENT_STA_GOT_IP
    // fired before our handler was registered. Check current state and start
    // the server if we're already on a network.
    if (wifi_connection_is_connected()) {
        server_start();
    }
}
