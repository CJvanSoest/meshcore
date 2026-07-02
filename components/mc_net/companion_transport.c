// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "companion_transport.h"
#include <stddef.h>  // offsetof
#include <stdio.h>
#include <string.h>
#include <sys/time.h>  // settimeofday for SET_DEVICE_TIME
#include <time.h>
#include <unistd.h>
#include "bsp/rtc.h"  // bsp_rtc_set_time
#include "channels.h"
#include "companion-radio-protocol/mc_companion.h"
#include "companion-radio-protocol/mc_companion_command_parser.h"
#include "companion-radio-protocol/mc_companion_serial_interface.h"
#include "contacts.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "identity.h"  // node_pub_key for SELF_INFO
#include "lora.h"
#include "nodes.h"  // node_mutex guards contacts[]
#include "radio.h"  // save_lora_config (apply radio params to the C6)
#include "settings_nvs.h"

static const char* TAG = "comp-tx";

// Active source + sender for the in-flight dispatch. Guarded by
// s_dispatch_mutex so a BLE write and a CDC read can't interleave their
// tags. Lazy-init in companion_dispatch_frame so callers don't need a
// separate init step.
static gps_source_t                s_current_source   = GPS_SRC_NONE;
static companion_response_sender_t s_current_sender   = NULL;
static SemaphoreHandle_t           s_dispatch_mutex   = NULL;
// Byte count of the in-flight command's args (opcode byte excluded). Lets a
// handler read fields the iPhone appends beyond our vendored struct (e.g. the
// tx_power field newer apps add to SET_RADIO_PARAMS). Set per BLE frame.
static size_t                      s_current_args_len = 0;

#define COMPANION_TASK_STACK 6144
#define COMPANION_TASK_PRIO  3
// 16 byte read chunk: small enough that low-latency single-byte pushes still
// dispatch promptly, large enough that a full 13-byte LATLON frame is one read.
#define READ_CHUNK_BYTES     16

// Build [response_code | args] and route through the registered sender. The
// sender adds whatever transport-level framing it needs (BLE notify adds
// none; a future serial response transport would prepend '>' + uint16_le).
// No-op when no sender is active (CDC currently has no response path).
void companion_send_response(uint8_t response_code, const void* args, size_t args_len) {
    if (!s_current_sender) return;
    uint8_t buf[1 + MESHCORE_COMPANION_MAX_PAYLOAD_SIZE];
    if (1 + args_len > sizeof(buf)) {
        ESP_LOGW(TAG, "response %u too large: %u B (max %u)", response_code, (unsigned)(1 + args_len),
                 (unsigned)sizeof(buf));
        return;
    }
    buf[0] = response_code;
    if (args_len > 0 && args) memcpy(&buf[1], args, args_len);
    s_current_sender(buf, 1 + args_len);
}

// Quick OK helper -- the iPhone app expects an OK ack on every write command.
static void send_ok(uint32_t result) {
    companion_resp_ok_args_t ok = {.result = result};
    companion_send_response(COMPANION_RESPONSE_CODE_OK, &ok, sizeof(ok));
}

// ── Opcode handlers ────────────────────────────────────────────────────────
static void handle_set_advert_latlon(const companion_cmd_set_advert_latlon_args_t* a) {
    int32_t lat = a->latitude;
    int32_t lon = a->longitude;
    // Same range check as upstream companion_radio MyMesh.cpp -- reject
    // anything outside the WGS84 envelope. Untrusted host could send
    // garbage; we don't want it landing in adverts.
    if (lat > 90 * 1000000 || lat < -90 * 1000000 || lon > 180 * 1000000 || lon < -180 * 1000000) {
        ESP_LOGW(TAG, "LATLON out of range: lat=%ld lon=%ld", (long)lat, (long)lon);
        companion_resp_err_args_t err = {.error_code = COMPANION_ERROR_CODE_ILLEGAL_ARG};
        companion_send_response(COMPANION_RESPONSE_CODE_ERR, &err, sizeof(err));
        return;
    }
    gps_lat_e6         = lat;
    gps_lon_e6         = lon;
    gps_position_valid = true;
    gps_last_source    = s_current_source;
    save_gps_coords();
    ESP_LOGI(TAG, "LATLON src=%u accepted: %.6f, %.6f", (unsigned)s_current_source, (double)lat / 1e6,
             (double)lon / 1e6);
    send_ok(0);
}

static void handle_device_query(void) {
    companion_resp_device_info_args_t info = {0};
    // Mirror upstream MeshCore companion_radio (MyMesh.h FIRMWARE_VER_CODE):
    // v1.15.0 → ver_code 11. iPhone gates Position Settings + Telemetry
    // behind ver_code >= 6 (v1.6.0+), so claiming 11 unlocks those screens
    // and stays future-compatible until upstream bumps further.
    info.firmware_version_code             = 11;
    info.max_contacts                      = 200;           // wire format: divide by 2 → max 100 real contacts
    info.max_group_channels                = CHANNELS_MAX;  // matches channels[] capacity
    const esp_app_desc_t* desc             = esp_app_get_description();
    if (desc) {
        strncpy(info.firmware_build_date, desc->date, sizeof(info.firmware_build_date) - 1);
    }
    // Report the upstream protocol version string the iPhone app expects.
    // Our actual app version lives in the build_date / settings UI -- this
    // field is the compatibility tag the app keys off.
    strncpy(info.firmware_version, "v1.15.0", sizeof(info.firmware_version) - 1);
    strncpy(info.board_manufacturer_name, "Nicolai Electronics Tanmatsu", sizeof(info.board_manufacturer_name) - 1);
    companion_send_response(COMPANION_RESPONSE_CODE_DEVICE_INFO, &info, sizeof(info));
}

static void handle_get_custom_vars(void) {
    // Upstream format: comma-separated "name:value" pairs in the data blob.
    // The iPhone app's Position Settings page reads the `gps` key to decide
    // whether GPS mode options are available -- absent → "Not Supported".
    // We always advertise gps support (PA1010D on QWIIC); the value tracks
    // ble_gps_pref so the iPhone toggle persists across reconnects.
    const char* gps_val = ble_gps_pref ? "gps:1" : "gps:0";
    companion_send_response(COMPANION_RESPONSE_CODE_CUSTOM_VARS, gps_val, strlen(gps_val));
}

static void handle_set_custom_var(const companion_cmd_set_custom_var_args_t* a, size_t args_len) {
    // Payload is a NUL-or-len-terminated "name:value" string. Copy locally so
    // we can mutate (replace ':' with NUL) without touching upstream's
    // parser buffer.
    if (args_len == 0) {
        send_ok(0);
        return;
    }
    char   buf[80] = {0};
    size_t n       = args_len < sizeof(buf) - 1 ? args_len : sizeof(buf) - 1;
    memcpy(buf, a->setting_value, n);
    buf[n] = '\0';
    // Strip trailing NULs that some clients append.
    while (n > 0 && buf[n - 1] == '\0') n--;

    char* sep = strchr(buf, ':');
    if (!sep) {
        send_ok(0);
        return;
    }
    *sep              = '\0';
    const char* name  = buf;
    const char* value = sep + 1;
    if (strcmp(name, "gps") == 0) {
        bool new_pref = (value[0] == '1');
        if (new_pref != ble_gps_pref) {
            ble_gps_pref = new_pref;
            save_ble_gps_pref();
        }
        ESP_LOGI(TAG, "SET_CUSTOM_VAR gps=%d", (int)ble_gps_pref);
    } else {
        ESP_LOGI(TAG, "SET_CUSTOM_VAR ignored: %s=%s", name, value);
    }
    send_ok(0);
}

static void handle_app_start(void) {
    // SELF_INFO -- snapshot of our advertising / radio state so the iPhone
    // app can pre-fill its UI. Mirrors what upstream companion_radio fills.
    companion_resp_self_info_args_t info = {0};
    info.adv_type                        = COMPANION_ADV_TYPE_CHAT;
    info.configured_tx_power             = (uint8_t)lora_cfg.power;
    info.maximum_tx_power                = 30;  // SX1262 ceiling
    memcpy(info.public_key, node_pub_key, sizeof(info.public_key));
    info.position_latitude   = gps_position_valid ? gps_lat_e6 : 0;
    info.position_longitude  = gps_position_valid ? gps_lon_e6 : 0;
    info.multi_acks          = 0;
    info.advert_loc_policy   = advert_loc_policy;
    info.telemetry_mode      = 0;
    info.manual_add_contacts = 0;
    info.frequency           = lora_cfg.frequency / 1000u;            // Hz -> kHz (app shows value/1000 as MHz)
    info.bandwidth           = (uint32_t)lora_cfg.bandwidth * 1000u;  // kHz → Hz for wire fmt
    info.spreading_factor    = lora_cfg.spreading_factor;
    info.coding_rate         = lora_cfg.coding_rate;
    const char* name         = lora_advert_name[0] ? lora_advert_name : owner_name;
    strncpy(info.node_name, name, sizeof(info.node_name) - 1);
    // Trim trailing NUL padding: upstream sends only up to strlen(node_name)
    // bytes (see MyMesh.cpp), and the iPhone app reads the full payload
    // verbatim -- if we send the 112-byte fixed field, the trailing zeros
    // render as garbled characters after the name in the Settings panel.
    size_t name_len   = strnlen(info.node_name, sizeof(info.node_name));
    size_t header_len = (size_t)((char*)info.node_name - (char*)&info);
    companion_send_response(COMPANION_RESPONSE_CODE_SELF_INFO, &info, header_len + name_len);
}

static void handle_set_other_params(const companion_cmd_set_other_params_args_t* a) {
    // Persist advert_location_policy so the "Share my position in adverts"
    // checkbox round-trips correctly. Other fields (manual_add_contacts,
    // flags, multi_acks) aren't surfaced in our Settings UI yet -- accept
    // and forget. iPhone treats OK as "saved", which is what we want.
    if (a->advert_location_policy != advert_loc_policy) {
        advert_loc_policy = a->advert_location_policy;
        save_advert_loc_policy();
    }
    ESP_LOGI(TAG, "SET_OTHER_PARAMS manual_add=%u flags=%02X advloc=%u multi_acks=%u", a->manual_add_contacts, a->flags,
             a->advert_location_policy, a->multi_acks);
    send_ok(0);
}

static void handle_get_device_time(void) {
    companion_resp_current_time_args_t t = {.current_time = (uint32_t)time(NULL)};
    companion_send_response(COMPANION_RESPONSE_CODE_CURRENT_TIME, &t, sizeof(t));
}

static void handle_get_contacts(void) {
    // These handlers run INLINE on the small nimble_host task stack (BLE rx
    // callback -> companion_dispatch_raw), so a big buffer in the stack frame
    // overflows it -- a 928 B snapshot here blue-screened the device on sync.
    // dispatch_locked serializes every dispatch via s_dispatch_mutex, so
    // file-static scratch is safe (no re-entrancy) and keeps the frame small.
    static contact_t           snap[MAX_CONTACTS];
    static companion_contact_t rc;

    // Snapshot contacts[] under node_mutex (shared with the RX sink), then emit
    // outside the lock so the per-contact BLE notifies don't hold it.
    int n = 0;
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        n = contact_count < MAX_CONTACTS ? contact_count : MAX_CONTACTS;
        memcpy(snap, contacts, (size_t)n * sizeof(contact_t));
        xSemaphoreGive(node_mutex);
    }

    uint32_t now = (uint32_t)time(NULL);

    companion_resp_contacts_start_t start = {.count = (uint32_t)n};
    companion_send_response(COMPANION_RESPONSE_CODE_CONTACTS_START, &start, sizeof(start));
    for (int i = 0; i < n; i++) {
        memset(&rc, 0, sizeof(rc));
        memcpy(rc.public_key, snap[i].pub_key, sizeof(rc.public_key));
        rc.type         = snap[i].role;
        rc.flags        = snap[i].flags;
        rc.out_path_len = -1;  // no stored route -> flood
        strncpy(rc.name, snap[i].alias, sizeof(rc.name) - 1);
        // We don't track per-contact advert/modified times yet; stamp with the
        // current time so the app doesn't treat them as stale and hide them.
        rc.last_advert_timestamp = now;
        rc.last_modified         = now;
        // Send the FULL record (incl. the trailing gps/last_modified fields) --
        // the app expects a complete contact.
        companion_send_response(COMPANION_RESPONSE_CODE_CONTACT, &rc, sizeof(rc));
        // Pace the burst: these notifies fire back-to-back from the BLE host
        // task, and NimBLE's mbuf pool drops (rc!=0) when it fills faster than
        // the transport drains it -- which silently lost contacts. A short
        // delay lets the pool recover between notifies. Harmless on the CDC path.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    companion_resp_end_of_contacts_t end = {.since = now};
    companion_send_response(COMPANION_RESPONSE_CODE_END_OF_CONTACTS, &end, sizeof(end));
}

static void handle_get_batt_and_storage(void) {
    // We don't expose battery to the app yet; report -1 (unknown) which
    // upstream treats as "device didn't measure". Storage fields optional.
    companion_resp_batt_and_storage_args_t b = {.battery_level = -1, .storage_used_kb = 0, .storage_total_kb = 0};
    companion_send_response(COMPANION_RESPONSE_CODE_BATT_AND_STORAGE, &b, sizeof(b));
}

static void handle_sync_next_message(void) {
    // We don't yet sync chat/contact messages over BLE. Tell the app there
    // is nothing pending; it'll stop polling. Real sync arrives in a later
    // PR once we wire chat_msg / contact-message ring buffers to BLE.
    companion_send_response(COMPANION_RESPONSE_CODE_NO_MORE_MESSAGES, NULL, 0);
}

static void handle_get_channel(const companion_cmd_get_channel_args_t* a) {
    // Clients (iPhone app, MeshMapper) walk slot indices and need two
    // distinct signals:
    //   * idx out of range -> ERR NOT_FOUND  (end-of-enumeration, stops walk)
    //   * idx in range but unused -> CHANNEL_INFO with empty name/secret
    //                                (signals "this slot is free, you may
    //                                 write here")
    // Returning ERR for empty-but-in-range was wrong: MeshMapper read that
    // as "device has no free slots" and refused to set up its own channel.
    if (a->channel_idx >= CHANNELS_MAX) {
        companion_resp_err_args_t err = {.error_code = COMPANION_ERROR_CODE_NOT_FOUND};
        companion_send_response(COMPANION_RESPONSE_CODE_ERR, &err, sizeof(err));
        return;
    }
    companion_resp_channel_info_args_t info = {0};
    info.channel_idx                        = a->channel_idx;
    if (channels[a->channel_idx].active) {
        const channel_t* ch = &channels[a->channel_idx];
        strncpy((char*)info.channel_name, ch->name, sizeof(info.channel_name) - 1);
        memcpy(info.channel_secret, ch->secret, sizeof(info.channel_secret));
    }
    companion_send_response(COMPANION_RESPONSE_CODE_CHANNEL_INFO, &info, sizeof(info));
}

static void send_err(uint8_t code) {
    companion_resp_err_args_t err = {.error_code = code};
    companion_send_response(COMPANION_RESPONSE_CODE_ERR, &err, sizeof(err));
}

static void handle_set_channel(const companion_cmd_set_channel_args_t* a) {
    // The app enumerates our slots via GET_CHANNEL and writes back to a
    // SPECIFIC channel_idx, so honour it. (Auto-slotting made the app read back
    // the wrong slot -> "secret 000000" until a reconnect re-enumerated.)
    // An empty name or an all-zero secret means "clear this slot" (delete).
    uint8_t zero[CHANNEL_SECRET_LEN] = {0};
    bool    is_delete = (a->channel_name[0] == '\0') || (memcmp(a->channel_secret, zero, sizeof(zero)) == 0);
    if (is_delete) {
        if (channels_remove(a->channel_idx)) {
            ESP_LOGI(TAG, "SET_CHANNEL removed slot %u", a->channel_idx);
            send_ok(0);
        } else {
            send_err(COMPANION_ERROR_CODE_ILLEGAL_ARG);  // idx 0 (Public) or already empty
        }
        return;
    }
    char name[CHANNEL_NAME_MAX_LEN + 1] = {0};
    strncpy(name, (const char*)a->channel_name, sizeof(name) - 1);  // 32B field -> our 23B cap
    if (!channels_set_at(a->channel_idx, name, a->channel_secret)) {
        send_err(COMPANION_ERROR_CODE_ILLEGAL_ARG);  // idx 0 (Public) or out of range
        return;
    }
    ESP_LOGI(TAG, "SET_CHANNEL '%s' -> slot %u", name, a->channel_idx);
    send_ok(0);
}

static void handle_add_update_contact(const companion_contact_t* a) {
    // contacts[] is shared with the RX sink -> hold node_mutex across the
    // find/mutate/save. The contact_*() helpers don't lock themselves.
    char alias[CONTACT_ALIAS_LEN] = {0};
    strncpy(alias, a->name, sizeof(alias) - 1);  // 32B field -> our 24B cap
    uint8_t code = COMPANION_ERROR_CODE_TABLE_FULL;
    bool    ok   = false;
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        int idx = contact_find(a->public_key);
        if (idx >= 0) {
            // Update in place: contact_ensure() is add-only, so overwrite here.
            strncpy(contacts[idx].alias, alias, CONTACT_ALIAS_LEN - 1);
            contacts[idx].alias[CONTACT_ALIAS_LEN - 1] = '\0';
            contacts[idx].role                         = a->type;
            contacts_save();
            ok = true;
        } else {
            ok = (contact_ensure(a->public_key, alias, a->type) > 0);  // -1 = table full
        }
        xSemaphoreGive(node_mutex);
    } else {
        code = COMPANION_ERROR_CODE_BAD_STATE;
    }
    if (ok) {
        ESP_LOGI(TAG, "ADD_UPDATE_CONTACT '%s' role=%u", alias, a->type);
        send_ok(0);
    } else {
        send_err(code);
    }
}

static void handle_remove_contact(const companion_cmd_remove_contact_args_t* a) {
    // Only remove a contact that exists -- contact_toggle() would otherwise
    // ADD the unknown pubkey. Hold node_mutex like the add path.
    bool    done = false;
    uint8_t code = COMPANION_ERROR_CODE_NOT_FOUND;
    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (contact_find(a->pub_key) >= 0) {
            contact_toggle(a->pub_key, NULL, 0);  // removes existing + persists
            done = true;
        }
        xSemaphoreGive(node_mutex);
    } else {
        code = COMPANION_ERROR_CODE_BAD_STATE;
    }
    if (done) {
        ESP_LOGI(TAG, "REMOVE_CONTACT ok");
        send_ok(0);
    } else {
        send_err(code);
    }
}

static void handle_set_advert_name(const companion_cmd_set_advert_name_args_t* a) {
    // Maps to the advert name (lora_advert_name), which shadows owner_name in
    // adverts + SELF_INFO -- non-destructive to the on-device owner identity.
    char name[sizeof(lora_advert_name)] = {0};
    strncpy(name, a->advert_name, sizeof(name) - 1);  // 32B field, may be unterminated
    strncpy(lora_advert_name, name, sizeof(lora_advert_name) - 1);
    lora_advert_name[sizeof(lora_advert_name) - 1] = '\0';
    save_lora_advert_name();
    ESP_LOGI(TAG, "SET_ADVERT_NAME '%s'", lora_advert_name);
    send_ok(0);
}

static void handle_set_device_time(const companion_cmd_set_device_time_args_t* a) {
    // Reject 0 / obviously-bad timestamps -- the boot path uses the same guard.
    if (a->timestamp < 1000000000u) {
        send_err(COMPANION_ERROR_CODE_ILLEGAL_ARG);
        return;
    }
    struct timeval tv = {.tv_sec = (time_t)a->timestamp, .tv_usec = 0};
    settimeofday(&tv, NULL);         // update the P4 system clock immediately
    bsp_rtc_set_time(a->timestamp);  // persist into the C6 RTC (the boot source)
    identity_mark_time_synced();     // treat time as authoritative
    ESP_LOGI(TAG, "SET_DEVICE_TIME %lu", (unsigned long)a->timestamp);
    send_ok(0);
}

// save_lora_config() blocks ~2 s on the C6 and drops RX, so never call it on the
// dispatch (nimble_host) task. Apply on a short-lived worker; an in-flight guard
// stops overlapping reconfigs when the app mashes settings.
static volatile bool s_radio_apply_in_flight = false;

static void radio_apply_task(void* arg) {
    (void)arg;
    save_lora_config();  // pushes lora_cfg to the C6 + persists to NVS
    s_radio_apply_in_flight = false;
    vTaskDelete(NULL);
}

static void schedule_radio_apply(void) {
    if (s_radio_apply_in_flight) return;
    s_radio_apply_in_flight = true;
    if (xTaskCreate(radio_apply_task, "radio_apply", 4096, NULL, 3, NULL) != pdPASS) {
        s_radio_apply_in_flight = false;
        save_lora_to_nvs();  // couldn't spawn: at least persist; applies next boot
    }
}

static void handle_set_radio_params(const companion_cmd_set_radio_params_args_t* a) {
    // SF/CR round-trip verbatim via SELF_INFO; only freq/bandwidth need unit
    // conversion (inverse of handle_app_start). Basic sanity checks -- the
    // regulatory layer still clamps freq/power on apply.
    if (a->frequency == 0 || a->bandwidth == 0 || a->spreading_factor < 5 || a->spreading_factor > 12 ||
        a->coding_rate < 5 || a->coding_rate > 8) {
        send_err(COMPANION_ERROR_CODE_ILLEGAL_ARG);
        return;
    }
    lora_cfg.frequency        = a->frequency * 1000u;  // kHz -> Hz
    lora_cfg.bandwidth        = a->bandwidth / 1000u;  // Hz  -> kHz
    lora_cfg.spreading_factor = a->spreading_factor;
    lora_cfg.coding_rate      = a->coding_rate;
    // Newer iPhone builds append tx_power as an 11th byte (our vendored struct
    // is the older 10-byte layout, so the app sends power via this command too).
    // Apply it only when present and a sane dBm value, so a non-power trailing
    // byte on some other client is ignored rather than nuking the power.
    if (s_current_args_len >= 11) {
        uint8_t p = ((const uint8_t*)a)[10];
        if (p >= 2 && p <= 30) lora_cfg.power = p;
    }
    ESP_LOGI(TAG, "SET_RADIO_PARAMS f=%luHz bw=%ukHz sf=%u cr=%u pwr=%u", (unsigned long)lora_cfg.frequency,
             lora_cfg.bandwidth, lora_cfg.spreading_factor, lora_cfg.coding_rate, lora_cfg.power);
    send_ok(0);  // ack before the blocking apply
    schedule_radio_apply();
}

static void handle_set_radio_tx_power(const companion_cmd_set_radio_tx_power_args_t* a) {
    lora_cfg.power = a->tx_power;
    ESP_LOGI(TAG, "SET_RADIO_TX_POWER %u", a->tx_power);
    send_ok(0);
    schedule_radio_apply();
}

// Dispatch one fully parsed command. Source tag comes from s_current_source
// which the calling transport sets via companion_dispatch_frame() before
// feeding bytes.
static void on_command(companion_command_packet_t* cmd, mc_companion_command_parser_error_t err) {
    if (err != COMPANION_COMMAND_PARSER_ERROR_NONE) {
        ESP_LOGW(TAG, "parse error %d", err);
        companion_resp_err_args_t e = {.error_code = COMPANION_ERROR_CODE_ILLEGAL_ARG};
        companion_send_response(COMPANION_RESPONSE_CODE_ERR, &e, sizeof(e));
        return;
    }

    switch (cmd->command) {
        case COMPANION_CMD_APP_START:
            handle_app_start();
            break;
        case COMPANION_CMD_DEVICE_QUERY:
            handle_device_query();
            break;
        case COMPANION_CMD_GET_DEVICE_TIME:
            handle_get_device_time();
            break;
        case COMPANION_CMD_GET_CONTACTS:
            handle_get_contacts();
            break;
        case COMPANION_CMD_GET_BATT_AND_STORAGE:
            handle_get_batt_and_storage();
            break;
        case COMPANION_CMD_SYNC_NEXT_MESSAGE:
            handle_sync_next_message();
            break;
        case COMPANION_CMD_GET_CHANNEL:
            handle_get_channel(&cmd->command_get_channel_args);
            break;
        case COMPANION_CMD_GET_CUSTOM_VARS:
            handle_get_custom_vars();
            break;
        case COMPANION_CMD_SET_CUSTOM_VAR: {
            // Parser's max_argument_length for SET_CUSTOM_VAR is the full
            // payload size, so cmd->args[..] holds the raw "name:value" blob.
            // We don't track the exact length here; pass a generous cap.
            handle_set_custom_var(&cmd->command_set_custom_var_args, MESHCORE_COMPANION_MAX_PAYLOAD_SIZE);
            break;
        }
        case COMPANION_CMD_SET_ADVERT_LATLON:
            handle_set_advert_latlon(&cmd->command_set_advert_latlon_args);
            break;
        case COMPANION_CMD_SET_OTHER_PARAMS:
            handle_set_other_params(&cmd->command_set_other_params_args);
            break;

        // ── Import from the iPhone app (settings / channels / contacts) ──
        case COMPANION_CMD_SET_CHANNEL:
            handle_set_channel(&cmd->command_set_channel_args);
            break;
        case COMPANION_CMD_ADD_UPDATE_CONTACT:
            handle_add_update_contact(&cmd->command_add_update_contact_args);
            break;
        case COMPANION_CMD_REMOVE_CONTACT:
            handle_remove_contact(&cmd->command_remove_contact_args);
            break;
        case COMPANION_CMD_SET_ADVERT_NAME:
            handle_set_advert_name(&cmd->command_set_advert_name_args);
            break;

        // ── Radio config + clock from the app ──
        case COMPANION_CMD_SET_DEVICE_TIME:
            handle_set_device_time(&cmd->command_set_device_time_args);
            break;
        case COMPANION_CMD_SET_RADIO_PARAMS:
            handle_set_radio_params(&cmd->command_set_radio_params_args);
            break;
        case COMPANION_CMD_SET_RADIO_TX_POWER:
            handle_set_radio_tx_power(&cmd->command_set_radio_tx_power_args);
            break;

        // Write commands the iPhone may send during normal use that we don't
        // implement yet -- ack OK so the app proceeds. Deliberate "polite stub"
        // so the handshake completes.
        case COMPANION_CMD_SEND_SELF_ADVERT:
        case COMPANION_CMD_RESET_PATH:
            ESP_LOGI(TAG, "stub-ack opcode %d", (int)cmd->command);
            send_ok(0);
            break;

        default:
            ESP_LOGI(TAG, "opcode %d not wired", (int)cmd->command);
            companion_resp_err_args_t e = {.error_code = COMPANION_ERROR_CODE_NOT_FOUND};
            companion_send_response(COMPANION_RESPONSE_CODE_ERR, &e, sizeof(e));
            break;
    }
}

// Shared mutex-protected entry: set source + sender, run the per-call body,
// then clear sender. Body is what differs between framed (CDC) and raw (BLE)
// transports.
static void dispatch_locked(gps_source_t src, companion_response_sender_t sender, void (*body)(const uint8_t*, size_t),
                            const uint8_t* buf, size_t len) {
    if (s_dispatch_mutex == NULL) {
        s_dispatch_mutex = xSemaphoreCreateMutex();
        if (s_dispatch_mutex == NULL) {
            ESP_LOGE(TAG, "mutex create failed; dropping frame");
            return;
        }
    }
    if (xSemaphoreTake(s_dispatch_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        ESP_LOGW(TAG, "dispatch mutex timeout; dropping frame");
        return;
    }
    s_current_source = src;
    s_current_sender = sender;
    body(buf, len);
    s_current_sender = NULL;
    xSemaphoreGive(s_dispatch_mutex);
}

static void body_framed(const uint8_t* buf, size_t len) {
    mc_companion_read_serial_command((uint8_t*)buf, len, on_command);
}

// Opcodes the iPhone sends with a wire layout the strict vendored parser
// rejects on length (a newer/longer client struct, or trailing padding). Their
// handlers validate their own args, so on a parse failure we copy the received
// bytes verbatim (bounded) and dispatch anyway -- otherwise the app toasts
// "illegal arg" on every loop. Do NOT add an opcode here whose handler does not
// self-validate (a too-short frame is zero-padded).
static bool opcode_lenient_recover(uint8_t op) {
    return op == COMPANION_CMD_SEND_SELF_ADVERT ||  // zero-arg + padding
           op == COMPANION_CMD_SET_DEVICE_TIME ||   // validates timestamp
           op == COMPANION_CMD_SET_RADIO_PARAMS ||  // 11-byte app struct vs our 10; validates
           op == COMPANION_CMD_SET_RADIO_TX_POWER;  // validates
}

static void body_raw(const uint8_t* buf, size_t len) {
    // BLE write boundary == command boundary. cmd[0] = opcode, cmd[1..] = args.
    if (len == 0) return;
    s_current_args_len                      = len - 1;
    companion_command_packet_t          pkt = {0};
    mc_companion_command_parser_error_t err = mc_companion_parse_command((uint8_t*)buf, (uint16_t)len, &pkt);
    if (err != COMPANION_COMMAND_PARSER_ERROR_NONE && opcode_lenient_recover(buf[0])) {
        // Copy the real args (bounded by the union size) instead of leaving them
        // zeroed, so a longer-than-expected frame still carries valid fields.
        size_t cap  = sizeof(pkt) - offsetof(companion_command_packet_t, args);
        size_t alen = s_current_args_len < cap ? s_current_args_len : cap;
        memcpy(pkt.args, buf + 1, alen);
        pkt.command = (companion_command_t)buf[0];
        on_command(&pkt, COMPANION_COMMAND_PARSER_ERROR_NONE);
        return;
    }
    on_command(&pkt, err);
}

void companion_dispatch_raw(const uint8_t* cmd, size_t len, gps_source_t src, companion_response_sender_t sender) {
    if (!cmd || len == 0) return;
    dispatch_locked(src, sender, body_raw, cmd, len);
}

void companion_dispatch_frame(const uint8_t* buf, size_t len, gps_source_t src, companion_response_sender_t sender) {
    if (!buf || len == 0) return;
    dispatch_locked(src, sender, body_framed, buf, len);
}

static void companion_transport_task(void* arg) {
    (void)arg;
    // Unbuffer stdin so single-byte writes from the host arrive promptly. The
    // default console buffering would otherwise wait for newline / 4 KB chunk.
    setvbuf(stdin, NULL, _IONBF, 0);

    uint8_t buf[READ_CHUNK_BYTES];
    ESP_LOGI(TAG, "reader task up (chunk=%d B)", READ_CHUNK_BYTES);
    while (1) {
        // read() on stdin is routed through the IDF VFS to whatever console
        // backend is active (USB-Serial-JTAG on Tanmatsu by default).
        // Blocks until at least 1 byte arrives.
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
            // No host attached / VFS returned no data -- yield instead of
            // burning CPU. Tick-rate (10 ms) is fine; companion pushes are
            // human-scale (seconds apart), not real-time.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        companion_dispatch_frame(buf, (size_t)n, GPS_SRC_CDC, NULL);
    }
}

bool companion_transport_init(void) {
    BaseType_t res =
        xTaskCreate(companion_transport_task, "comp-tx", COMPANION_TASK_STACK, NULL, COMPANION_TASK_PRIO, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    return true;
}
