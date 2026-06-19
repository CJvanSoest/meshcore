// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "companion_transport.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "companion-radio-protocol/mc_companion.h"
#include "companion-radio-protocol/mc_companion_command_parser.h"
#include "companion-radio-protocol/mc_companion_serial_interface.h"
#include "channels.h"
#include "identity.h"     // node_pub_key for SELF_INFO
#include "lora.h"
#include "settings_nvs.h"

static const char *TAG = "comp-tx";

// Active source + sender for the in-flight dispatch. Guarded by
// s_dispatch_mutex so a BLE write and a CDC read can't interleave their
// tags. Lazy-init in companion_dispatch_frame so callers don't need a
// separate init step.
static gps_source_t                s_current_source = GPS_SRC_NONE;
static companion_response_sender_t s_current_sender = NULL;
static SemaphoreHandle_t           s_dispatch_mutex = NULL;

#define COMPANION_TASK_STACK 6144
#define COMPANION_TASK_PRIO  3
// 16 byte read chunk: small enough that low-latency single-byte pushes still
// dispatch promptly, large enough that a full 13-byte LATLON frame is one read.
#define READ_CHUNK_BYTES     16

// Build [response_code | args] and route through the registered sender. The
// sender adds whatever transport-level framing it needs (BLE notify adds
// none; a future serial response transport would prepend '>' + uint16_le).
// No-op when no sender is active (CDC currently has no response path).
void companion_send_response(uint8_t response_code, const void *args, size_t args_len) {
    if (!s_current_sender) return;
    uint8_t  buf[1 + MESHCORE_COMPANION_MAX_PAYLOAD_SIZE];
    if (1 + args_len > sizeof(buf)) {
        ESP_LOGW(TAG, "response %u too large: %u B (max %u)",
                 response_code, (unsigned)(1 + args_len), (unsigned)sizeof(buf));
        return;
    }
    buf[0] = response_code;
    if (args_len > 0 && args) memcpy(&buf[1], args, args_len);
    s_current_sender(buf, 1 + args_len);
}

// Quick OK helper -- the iPhone app expects an OK ack on every write command.
static void send_ok(uint32_t result) {
    companion_resp_ok_args_t ok = { .result = result };
    companion_send_response(COMPANION_RESPONSE_CODE_OK, &ok, sizeof(ok));
}

// ── Opcode handlers ────────────────────────────────────────────────────────
static void handle_set_advert_latlon(const companion_cmd_set_advert_latlon_args_t *a) {
    int32_t lat = a->latitude;
    int32_t lon = a->longitude;
    // Same range check as upstream companion_radio MyMesh.cpp -- reject
    // anything outside the WGS84 envelope. Untrusted host could send
    // garbage; we don't want it landing in adverts.
    if (lat > 90 * 1000000 || lat < -90 * 1000000 ||
        lon > 180 * 1000000 || lon < -180 * 1000000) {
        ESP_LOGW(TAG, "LATLON out of range: lat=%ld lon=%ld", (long)lat, (long)lon);
        companion_resp_err_args_t err = { .error_code = COMPANION_ERROR_CODE_ILLEGAL_ARG };
        companion_send_response(COMPANION_RESPONSE_CODE_ERR, &err, sizeof(err));
        return;
    }
    gps_lat_e6         = lat;
    gps_lon_e6         = lon;
    gps_position_valid = true;
    gps_last_source    = s_current_source;
    save_gps_coords();
    ESP_LOGI(TAG, "LATLON src=%u accepted: %.6f, %.6f",
             (unsigned)s_current_source, (double)lat / 1e6, (double)lon / 1e6);
    send_ok(0);
}

static void handle_device_query(void) {
    companion_resp_device_info_args_t info = {0};
    // Mirror upstream MeshCore companion_radio (MyMesh.h FIRMWARE_VER_CODE):
    // v1.15.0 → ver_code 11. iPhone gates Position Settings + Telemetry
    // behind ver_code >= 6 (v1.6.0+), so claiming 11 unlocks those screens
    // and stays future-compatible until upstream bumps further.
    info.firmware_version_code = 11;
    info.max_contacts          = 200;  // wire format: divide by 2 → max 100 real contacts
    info.max_group_channels    = CHANNELS_MAX;  // matches channels[] capacity
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        strncpy(info.firmware_build_date, desc->date, sizeof(info.firmware_build_date) - 1);
    }
    // Report the upstream protocol version string the iPhone app expects.
    // Our actual app version lives in the build_date / settings UI -- this
    // field is the compatibility tag the app keys off.
    strncpy(info.firmware_version, "v1.15.0", sizeof(info.firmware_version) - 1);
    strncpy(info.board_manufacturer_name, "Nicolai Electronics Tanmatsu",
            sizeof(info.board_manufacturer_name) - 1);
    companion_send_response(COMPANION_RESPONSE_CODE_DEVICE_INFO, &info, sizeof(info));
}

static void handle_get_custom_vars(void) {
    // Upstream format: comma-separated "name:value" pairs in the data blob.
    // The iPhone app's Position Settings page reads the `gps` key to decide
    // whether GPS mode options are available -- absent → "Not Supported".
    // We always advertise gps support (PA1010D on QWIIC); the value tracks
    // ble_gps_pref so the iPhone toggle persists across reconnects.
    const char *gps_val = ble_gps_pref ? "gps:1" : "gps:0";
    companion_send_response(COMPANION_RESPONSE_CODE_CUSTOM_VARS,
                            gps_val, strlen(gps_val));
}

static void handle_set_custom_var(const companion_cmd_set_custom_var_args_t *a, size_t args_len) {
    // Payload is a NUL-or-len-terminated "name:value" string. Copy locally so
    // we can mutate (replace ':' with NUL) without touching upstream's
    // parser buffer.
    if (args_len == 0) { send_ok(0); return; }
    char  buf[80] = {0};
    size_t n = args_len < sizeof(buf) - 1 ? args_len : sizeof(buf) - 1;
    memcpy(buf, a->setting_value, n);
    buf[n] = '\0';
    // Strip trailing NULs that some clients append.
    while (n > 0 && buf[n - 1] == '\0') n--;

    char *sep = strchr(buf, ':');
    if (!sep) { send_ok(0); return; }
    *sep = '\0';
    const char *name  = buf;
    const char *value = sep + 1;
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
    info.adv_type            = COMPANION_ADV_TYPE_CHAT;
    info.configured_tx_power = (uint8_t)lora_cfg.power;
    info.maximum_tx_power    = 30;  // SX1262 ceiling
    memcpy(info.public_key, node_pub_key, sizeof(info.public_key));
    info.position_latitude  = gps_position_valid ? gps_lat_e6 : 0;
    info.position_longitude = gps_position_valid ? gps_lon_e6 : 0;
    info.multi_acks         = 0;
    info.advert_loc_policy  = advert_loc_policy;
    info.telemetry_mode     = 0;
    info.manual_add_contacts = 0;
    info.frequency          = lora_cfg.frequency;
    info.bandwidth          = (uint32_t)lora_cfg.bandwidth * 1000u;  // kHz → Hz for wire fmt
    info.spreading_factor   = lora_cfg.spreading_factor;
    info.coding_rate        = lora_cfg.coding_rate;
    const char *name = lora_advert_name[0] ? lora_advert_name : owner_name;
    strncpy(info.node_name, name, sizeof(info.node_name) - 1);
    // Trim trailing NUL padding: upstream sends only up to strlen(node_name)
    // bytes (see MyMesh.cpp), and the iPhone app reads the full payload
    // verbatim -- if we send the 112-byte fixed field, the trailing zeros
    // render as garbled characters after the name in the Settings panel.
    size_t name_len   = strnlen(info.node_name, sizeof(info.node_name));
    size_t header_len = (size_t)((char *)info.node_name - (char *)&info);
    companion_send_response(COMPANION_RESPONSE_CODE_SELF_INFO, &info, header_len + name_len);
}

static void handle_set_other_params(const companion_cmd_set_other_params_args_t *a) {
    // Persist advert_location_policy so the "Share my position in adverts"
    // checkbox round-trips correctly. Other fields (manual_add_contacts,
    // flags, multi_acks) aren't surfaced in our Settings UI yet -- accept
    // and forget. iPhone treats OK as "saved", which is what we want.
    if (a->advert_location_policy != advert_loc_policy) {
        advert_loc_policy = a->advert_location_policy;
        save_advert_loc_policy();
    }
    ESP_LOGI(TAG, "SET_OTHER_PARAMS manual_add=%u flags=%02X advloc=%u multi_acks=%u",
             a->manual_add_contacts, a->flags, a->advert_location_policy, a->multi_acks);
    send_ok(0);
}

static void handle_get_device_time(void) {
    companion_resp_current_time_args_t t = { .current_time = (uint32_t)time(NULL) };
    companion_send_response(COMPANION_RESPONSE_CODE_CURRENT_TIME, &t, sizeof(t));
}

static void handle_get_contacts(void) {
    // Minimal: report zero contacts. Future PR will iterate contacts_get()
    // and emit COMPANION_RESPONSE_CODE_CONTACT for each. iPhone tolerates an
    // empty list -- handshake completes either way.
    companion_resp_contacts_start_t start = { .count = 0 };
    companion_send_response(COMPANION_RESPONSE_CODE_CONTACTS_START, &start, sizeof(start));
    companion_resp_end_of_contacts_t end = { .since = (uint32_t)time(NULL) };
    companion_send_response(COMPANION_RESPONSE_CODE_END_OF_CONTACTS, &end, sizeof(end));
}

static void handle_get_batt_and_storage(void) {
    // We don't expose battery to the app yet; report -1 (unknown) which
    // upstream treats as "device didn't measure". Storage fields optional.
    companion_resp_batt_and_storage_args_t b = { .battery_level = -1,
                                                  .storage_used_kb = 0,
                                                  .storage_total_kb = 0 };
    companion_send_response(COMPANION_RESPONSE_CODE_BATT_AND_STORAGE, &b, sizeof(b));
}

static void handle_sync_next_message(void) {
    // We don't yet sync chat/contact messages over BLE. Tell the app there
    // is nothing pending; it'll stop polling. Real sync arrives in a later
    // PR once we wire chat_msg / contact-message ring buffers to BLE.
    companion_send_response(COMPANION_RESPONSE_CODE_NO_MORE_MESSAGES, NULL, 0);
}

static void handle_get_channel(const companion_cmd_get_channel_args_t *a) {
    // Clients (iPhone app, MeshMapper) walk slot indices and need two
    // distinct signals:
    //   * idx out of range -> ERR NOT_FOUND  (end-of-enumeration, stops walk)
    //   * idx in range but unused -> CHANNEL_INFO with empty name/secret
    //                                (signals "this slot is free, you may
    //                                 write here")
    // Returning ERR for empty-but-in-range was wrong: MeshMapper read that
    // as "device has no free slots" and refused to set up its own channel.
    if (a->channel_idx >= CHANNELS_MAX) {
        companion_resp_err_args_t err = { .error_code = COMPANION_ERROR_CODE_NOT_FOUND };
        companion_send_response(COMPANION_RESPONSE_CODE_ERR, &err, sizeof(err));
        return;
    }
    companion_resp_channel_info_args_t info = {0};
    info.channel_idx = a->channel_idx;
    if (channels[a->channel_idx].active) {
        const channel_t *ch = &channels[a->channel_idx];
        strncpy((char *)info.channel_name, ch->name, sizeof(info.channel_name) - 1);
        memcpy(info.channel_secret, ch->secret, sizeof(info.channel_secret));
    }
    companion_send_response(COMPANION_RESPONSE_CODE_CHANNEL_INFO, &info, sizeof(info));
}

// Dispatch one fully parsed command. Source tag comes from s_current_source
// which the calling transport sets via companion_dispatch_frame() before
// feeding bytes.
static void on_command(companion_command_packet_t *cmd, mc_companion_command_parser_error_t err) {
    if (err != COMPANION_COMMAND_PARSER_ERROR_NONE) {
        ESP_LOGW(TAG, "parse error %d", err);
        companion_resp_err_args_t e = { .error_code = COMPANION_ERROR_CODE_ILLEGAL_ARG };
        companion_send_response(COMPANION_RESPONSE_CODE_ERR, &e, sizeof(e));
        return;
    }

    switch (cmd->command) {
        case COMPANION_CMD_APP_START:           handle_app_start(); break;
        case COMPANION_CMD_DEVICE_QUERY:        handle_device_query(); break;
        case COMPANION_CMD_GET_DEVICE_TIME:     handle_get_device_time(); break;
        case COMPANION_CMD_GET_CONTACTS:        handle_get_contacts(); break;
        case COMPANION_CMD_GET_BATT_AND_STORAGE: handle_get_batt_and_storage(); break;
        case COMPANION_CMD_SYNC_NEXT_MESSAGE:   handle_sync_next_message(); break;
        case COMPANION_CMD_GET_CHANNEL:         handle_get_channel(&cmd->command_get_channel_args); break;
        case COMPANION_CMD_GET_CUSTOM_VARS:     handle_get_custom_vars(); break;
        case COMPANION_CMD_SET_CUSTOM_VAR: {
            // Parser's max_argument_length for SET_CUSTOM_VAR is the full
            // payload size, so cmd->args[..] holds the raw "name:value" blob.
            // We don't track the exact length here; pass a generous cap.
            handle_set_custom_var(&cmd->command_set_custom_var_args,
                                  MESHCORE_COMPANION_MAX_PAYLOAD_SIZE);
            break;
        }
        case COMPANION_CMD_SET_ADVERT_LATLON:   handle_set_advert_latlon(&cmd->command_set_advert_latlon_args); break;
        case COMPANION_CMD_SET_OTHER_PARAMS:    handle_set_other_params(&cmd->command_set_other_params_args); break;

        // Write commands the iPhone may send during normal use that we don't
        // implement yet -- ack OK so the app proceeds. This is a deliberate
        // "polite stub" so the handshake completes; real handlers are a PR-2d
        // follow-up if/when we want chat / contacts / advert-tx control.
        case COMPANION_CMD_SET_ADVERT_NAME:
        case COMPANION_CMD_SEND_SELF_ADVERT:
        case COMPANION_CMD_SET_DEVICE_TIME:
        case COMPANION_CMD_RESET_PATH:
        case COMPANION_CMD_ADD_UPDATE_CONTACT:
        case COMPANION_CMD_REMOVE_CONTACT:
        case COMPANION_CMD_SET_RADIO_PARAMS:
        case COMPANION_CMD_SET_RADIO_TX_POWER:
            ESP_LOGI(TAG, "stub-ack opcode %d", (int)cmd->command);
            send_ok(0);
            break;

        default:
            ESP_LOGI(TAG, "opcode %d not wired", (int)cmd->command);
            companion_resp_err_args_t e = { .error_code = COMPANION_ERROR_CODE_NOT_FOUND };
            companion_send_response(COMPANION_RESPONSE_CODE_ERR, &e, sizeof(e));
            break;
    }
}

// Shared mutex-protected entry: set source + sender, run the per-call body,
// then clear sender. Body is what differs between framed (CDC) and raw (BLE)
// transports.
static void dispatch_locked(gps_source_t src, companion_response_sender_t sender,
                            void (*body)(const uint8_t *, size_t), const uint8_t *buf, size_t len) {
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

static void body_framed(const uint8_t *buf, size_t len) {
    mc_companion_read_serial_command((uint8_t *)buf, len, on_command);
}

static void body_raw(const uint8_t *buf, size_t len) {
    // BLE write boundary == command boundary. cmd[0] = opcode, cmd[1..] = args.
    if (len == 0) return;
    companion_command_packet_t pkt = {0};
    mc_companion_command_parser_error_t err =
        mc_companion_parse_command((uint8_t *)buf, (uint16_t)len, &pkt);
    if (err != COMPANION_COMMAND_PARSER_ERROR_NONE) {
        // Upstream parser is strict about arg length and bails without
        // setting pkt.command. iPhone sends opcodes 7 (SEND_SELF_ADVERT)
        // with one byte of padding the parser doesn't expect. For opcodes
        // we'd ack anyway, recover the opcode from the raw buffer and
        // dispatch as if the parse succeeded.
        uint8_t op = buf[0];
        if (op == COMPANION_CMD_SEND_SELF_ADVERT ||
            op == COMPANION_CMD_SET_DEVICE_TIME ||
            op == COMPANION_CMD_SET_RADIO_PARAMS ||
            op == COMPANION_CMD_ADD_UPDATE_CONTACT) {
            // These opcodes the iPhone sends with a wire layout the strict
            // upstream parser doesn't accept (newer client struct than our
            // vendored protocol header, or trailing padding). For opcodes
            // we'd stub-ack anyway, recover the opcode from buf[0] and
            // dispatch as if clean -- otherwise the iPhone toasts
            // "illegal arg" on every loop because we returned ERR.
            pkt.command = (companion_command_t)op;
            on_command(&pkt, COMPANION_COMMAND_PARSER_ERROR_NONE);
            return;
        }
    }
    on_command(&pkt, err);
}

void companion_dispatch_raw(const uint8_t *cmd, size_t len, gps_source_t src,
                            companion_response_sender_t sender) {
    if (!cmd || len == 0) return;
    dispatch_locked(src, sender, body_raw, cmd, len);
}

void companion_dispatch_frame(const uint8_t *buf, size_t len, gps_source_t src,
                              companion_response_sender_t sender) {
    if (!buf || len == 0) return;
    dispatch_locked(src, sender, body_framed, buf, len);
}

static void companion_transport_task(void *arg) {
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
    BaseType_t res = xTaskCreate(companion_transport_task, "comp-tx",
                                 COMPANION_TASK_STACK, NULL,
                                 COMPANION_TASK_PRIO, NULL);
    if (res != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    return true;
}
