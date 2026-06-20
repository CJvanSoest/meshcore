// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "ble_companion.h"
#include <stdint.h>
#include <string.h>
#include "companion_transport.h"
#include "esp_hosted_misc.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "settings_nvs.h"

static const char* TAG = "ble";

// Nordic UART service pattern that MeshCore upstream uses (see
// SerialBLEInterface.cpp). Matching these UUIDs means the iPhone MeshCore
// app, T-Beam companion, and any other off-the-shelf MeshCore peer can talk
// to us with no app-side changes.
//
// Service:   6E400001-B5A3-F393-E0A9-E50E24DCCA9E
// RX char:   6E400002-...  (peer writes frames to us)
// TX char:   6E400003-...  (we notify peer with responses, future PR)
//
// NimBLE stores UUIDs little-endian, so the byte arrays below are the reverse
// of the human-readable form.
static const ble_uuid128_t kSvcUuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t kRxUuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t kTxUuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static uint8_t  s_own_addr_type;
static uint16_t s_tx_attr_handle;
// Track the one active connection so we can route response notifies. NimBLE
// supports multiple in theory, but the iPhone MeshCore app pairs 1:1 and we
// only advertise undirected -- one peer at a time is fine.
#define BLE_CONN_INVALID 0xFFFF
static uint16_t s_active_conn = BLE_CONN_INVALID;

// Forward declarations
static int gap_event(struct ble_gap_event* event, void* arg);
static int rx_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg);
static int tx_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg);

// ── Default weak hooks for the UI (overridden in render code) ───────────────
__attribute__((weak)) void ble_companion_show_passkey(uint32_t passkey) {
    ESP_LOGI(TAG, "PAIRING PASSKEY: %06lu (override in UI)", (unsigned long)passkey);
}

__attribute__((weak)) void ble_companion_pair_done(bool success) {
    ESP_LOGI(TAG, "pair_done: %s", success ? "OK" : "FAIL");
}

// ── GATT table ──────────────────────────────────────────────────────────────
static const struct ble_gatt_chr_def kCharacteristics[] = {
    {
        // RX: peer writes frames here. Encrypted + authenticated write so
        // pairing is required before any LATLON push lands.
        .uuid      = &kRxUuid.u,
        .access_cb = rx_chr_access,
        .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN,
    },
    {
        // TX: we notify peer with responses. Read + Notify, encrypted.
        // PR-2b uses this only for the OK/ERR reply to a SET_ADVERT_LATLON;
        // future opcodes will stream contacts / channel state here.
        .uuid       = &kTxUuid.u,
        .access_cb  = tx_chr_access,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
        .val_handle = &s_tx_attr_handle,
    },
    {0}  // terminator
};

static const struct ble_gatt_svc_def kServices[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &kSvcUuid.u,
        .characteristics = kCharacteristics,
    },
    {0}  // terminator
};

// Send a framed response by notifying the TX characteristic on the active
// connection. Called by companion_send_response() during opcode dispatch via
// the s_current_sender pointer.
static void ble_send_response(const uint8_t* frame, size_t len) {
    if (s_active_conn == BLE_CONN_INVALID) {
        ESP_LOGW(TAG, "send_response with no active conn (len=%u)", (unsigned)len);
        return;
    }
    struct os_mbuf* om = ble_hs_mbuf_from_flat(frame, len);
    if (!om) {
        ESP_LOGE(TAG, "mbuf alloc failed for %u B", (unsigned)len);
        return;
    }
    int rc = ble_gatts_notify_custom(s_active_conn, s_tx_attr_handle, om);
    if (rc != 0) ESP_LOGW(TAG, "notify_custom rc=%d", rc);
}

// ── RX characteristic write: feed frames into the protocol parser ───────────
static int rx_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)attr_handle;
    (void)arg;
    // Verbose probe -- we suspect iPhone writes never reach us, so log every
    // access (read / write / unknown) until handshake works, then strip.
    ESP_LOGI(TAG, "rx_chr_access op=%d conn=%u", ctxt->op, conn_handle);
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    // Drain the mbuf chain into a contiguous buffer (max one frame).
    uint8_t  buf[256];
    uint16_t copied = 0;
    int      rc     = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &copied);
    if (rc != 0 || copied == 0) {
        ESP_LOGW(TAG, "rx flatten failed rc=%d copied=%u", rc, copied);
        return 0;
    }
    ESP_LOGI(TAG, "rx_chr_access write %u B: %02X %02X %02X %02X ...", copied, buf[0], copied > 1 ? buf[1] : 0,
             copied > 2 ? buf[2] : 0, copied > 3 ? buf[3] : 0);
    // Conn handle is captured globally on connect/disconnect; pass the
    // current peer's sender so responses route back to the same iPhone.
    // BLE ATT writes are unframed -- each write is one complete command
    // (opcode byte + args). Use the _raw entry point, not the framed one.
    (void)conn_handle;
    companion_dispatch_raw(buf, copied, GPS_SRC_BLE, ble_send_response);
    return 0;
}

// ── TX characteristic read: empty for now (notify-only in practice) ──────────
static int tx_chr_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void* arg) {
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    (void)ctxt;
    return 0;
}

// ── GAP advertising ────────────────────────────────────────────────────────
static void advertise(void) {
    struct ble_hs_adv_fields fields = {0};
    fields.flags                    = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present    = 1;
    fields.tx_pwr_lvl               = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.uuids128                 = (ble_uuid128_t*)&kSvcUuid;
    fields.num_uuids128             = 1;
    fields.uuids128_is_complete     = 1;
    // Advertise the device name from settings (owner_name) so the iPhone
    // shows it in the pairing list. Falls back to "Tanmatsu" if not set yet.
    const char* name                = (owner_name[0] && owner_name[0] != '(') ? owner_name : "Tanmatsu";
    fields.name                     = (uint8_t*)name;
    fields.name_len                 = (uint8_t)strlen(name);
    fields.name_is_complete         = 1;
    int rc                          = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = {0};
    params.conn_mode                 = BLE_GAP_CONN_MODE_UND;
    params.disc_mode                 = BLE_GAP_DISC_MODE_GEN;
    // ~1 second advertising interval. Bluetooth time units are 0.625 ms, so
    // 1600 = 1000 ms. Tradeoff: longer interval = lower average current draw
    // (BLE peripheral is dominated by adv-burst frequency), slower iPhone
    // discovery. 1 s is the upstream MeshCore default and fast enough for
    // pair-and-forget usage.
    params.itvl_min                  = 1600;
    params.itvl_max                  = 1600;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0)
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
    else
        ESP_LOGI(TAG, "advertising as \"%s\"", name);
}

// ── GAP event handler ──────────────────────────────────────────────────────
static int gap_event(struct ble_gap_event* event, void* arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "connect status=%d handle=%u", event->connect.status, event->connect.conn_handle);
            if (event->connect.status == 0) {
                s_active_conn = event->connect.conn_handle;
            } else {
                advertise();  // re-advertise on fail
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
            s_active_conn = BLE_CONN_INVALID;
            advertise();
            break;

        case BLE_GAP_EVENT_ENC_CHANGE: {
            // Probe sec_state so we know if the connection is authenticated.
            // WRITE_AUTHEN characteristic flag requires authenticated=1; a
            // bond from passkey-display pairing should set that. If iPhone
            // writes fail, this is the first thing to suspect.
            struct ble_gap_conn_desc desc;
            int                      rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(TAG, "enc_change status=%d enc=%u auth=%u bonded=%u key_size=%u", event->enc_change.status,
                         desc.sec_state.encrypted, desc.sec_state.authenticated, desc.sec_state.bonded,
                         desc.sec_state.key_size);
            } else {
                ESP_LOGI(TAG, "enc_change status=%d (conn_find rc=%d)", event->enc_change.status, rc);
            }
            ble_companion_pair_done(event->enc_change.status == 0);
            break;
        }

        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            // SMP wants us to either display, input, or accept a passkey.
            // io_cap is DISPLAY_ONLY → only DISP action is expected.
            struct ble_sm_io io = {0};
            if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
                // Generate a random 6-digit passkey, log it, hand to SMP.
                uint32_t        pk = 0;
                // ble_hs has no random helper here; use IDF's esp_random.
                extern uint32_t esp_random(void);
                pk         = esp_random() % 1000000;
                io.action  = BLE_SM_IOACT_DISP;
                io.passkey = pk;
                ble_companion_show_passkey(pk);
                int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
                ESP_LOGI(TAG, "passkey injected rc=%d (pk=%06lu)", rc, (unsigned long)pk);
            } else {
                ESP_LOGW(TAG, "unexpected passkey action=%d", event->passkey.params.action);
            }
            break;
        }

        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "subscribe handle=%u cur_notify=%d", event->subscribe.attr_handle,
                     event->subscribe.cur_notify);
            break;

        default:
            break;
    }
    return 0;
}

// ── Host stack lifecycle callbacks ─────────────────────────────────────────
static void on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto rc=%d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "host reset reason=%d", reason);
}

static void host_task(void* arg) {
    (void)arg;
    nimble_port_run();  // returns only after nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ── Public init ────────────────────────────────────────────────────────────
bool ble_companion_init(void) {
    // BT controller lives on the C6 via esp-hosted. Bring it up before NimBLE.
    esp_err_t res = esp_hosted_bt_controller_init();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_bt_controller_init failed: %d", res);
        return false;
    }
    res = esp_hosted_bt_controller_enable();
    if (res != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_bt_controller_enable failed: %d", res);
        return false;
    }

    nimble_port_init();

    // Host config. SC + bonding + MITM gives us LE Secure Connections with
    // Passkey Display Entry, persisted to NVS so re-pairing is silent.
    ble_hs_cfg.reset_cb          = on_reset;
    ble_hs_cfg.sync_cb           = on_sync;
    ble_hs_cfg.store_status_cb   = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 1;
    ble_hs_cfg.sm_sc             = 1;
    // Distribute encryption + ID keys both ways so reconnects can resolve.
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(kServices);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg rc=%d", rc);
        return false;
    }
    rc = ble_gatts_add_svcs(kServices);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs rc=%d", rc);
        return false;
    }

    // Device name in the GAP "name" attribute. Matches what we advertise.
    const char* name = (owner_name[0] && owner_name[0] != '(') ? owner_name : "Tanmatsu";
    ble_svc_gap_device_name_set(name);

    // Bond persistence -- nvs_flash_init() must already be done by main.c.
    extern void ble_store_config_init(void);
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "init OK (name=\"%s\")", name);
    return true;
}
