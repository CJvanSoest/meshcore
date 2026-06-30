// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "input.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "ble_companion.h"
#include "bsp/device.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "channel_share.h"
#include "channels.h"
#include "chat.h"
#include "contacts.h"
#include "coverage.h"
#include "diag.h"
#include "emoji_table.h"  // EMOJI_SET / emoji_entry_t (picker selection)
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "gps.h"
#include "gps_task.h"
#include "history.h"
#include "http_server.h"
#include "identity.h"
#include "map.h"
#include "mc_rx.h"
#include "nodes.h"
#include "radio.h"
#include "region_limits.h"
#include "render.h"
#include "render_internal.h"
#include "settings_nvs.h"
#include "sounds.h"
#include "ui_state.h"
#include "wifi_connection.h"

static const char* TAG = "input";

// Open the tile under the home-cursor: switch current_view to its target
// and initialise per-view modal state (DM inbox, channel list, etc.). TBD
// placeholder tiles report VIEW_HOME and are a no-op here.
static void open_home_tile(int idx) {
    home_action_t action = home_tile_action(idx);
    app_view_t    target = home_tile_target(idx);

    // Exit tile: return to the BadgeVMS launcher. Same proven path as the
    // ESC-on-home shortcut; the tile just makes it discoverable.
    if (action == HOME_ACTION_EXIT) {
        bsp_device_restart_to_launcher();
        return;
    }

    // Advert tile: drill straight into Settings -> Advert so the user
    // tweaks intervals + triggers manual sends in one place. The
    // "Send flood now" action row inside the Advert category replaces
    // the previous tap-to-send-immediately behaviour, which made it too
    // easy to accidentally spam the mesh.
    if (action == HOME_ACTION_OPEN_ADVERT) {
        int adv_cat = settings_category_for_field(FIELD_FLOOD_ADVERT_INT);
        if (adv_cat < 0) adv_cat = 0;
        settings_category_active    = adv_cat;
        // Cursor is in visible-slot space; Advert is hidden, so park the
        // cursor at slot 0 -- the user lands on the first visible tile
        // when they ESC back out of the Advert drilldown.
        settings_category_cursor    = 0;
        settings_category_list_mode = false;
        selected                    = FIELD_FLOOD_ADVERT_INT;
        current_view                = VIEW_SETTINGS;
        return;
    }

    if (target == VIEW_HOME) return;
    current_view = target;
    if (target == VIEW_CHAT) {
        dm_inbox_mode  = true;
        led_dm_pending = false;
        update_notification_led();
    } else if (target == VIEW_CHANNEL) {
        channel_list_mode   = true;
        channel_adding      = false;
        led_channel_pending = false;
        update_notification_led();
    } else if (target == VIEW_SETTINGS) {
        // Opening Settings from the home tile starts at the category list so
        // the user always sees the same top-level menu (Tab cycling preserves
        // last position; tile-open resets).
        settings_category_list_mode = true;
        settings_category_cursor    = 0;
        settings_scroll             = 0;
    }
    // QR tile: open the overlay on top of the nodes view AND remember the
    // origin so closing it bounces back to home instead of leaving the user
    // stranded in the nodes list.
    if (action == HOME_ACTION_OPEN_QR) {
        qr_overlay_active = true;
        qr_from_home      = true;
    }
}

// ── Settings: lookup current BW index ────────────────────────────────────────
int bw_index(void) {
    for (int i = 0; i < BW_COUNT; i++) {
        if (BW_OPTIONS[i] == (uint16_t)lora_cfg.bandwidth) return i;
    }
    return 7;
}

// ── Settings: step a numeric/enum field ──────────────────────────────────────
void field_adjust(int field, int delta) {
    switch (field) {
        case FIELD_FREQ:
            if (delta > 0 && lora_cfg.frequency < 870000000u) lora_cfg.frequency += 100000;
            if (delta < 0 && lora_cfg.frequency > 863000000u) lora_cfg.frequency -= 100000;
            break;
        case FIELD_SF: {
            int sf = (int)lora_cfg.spreading_factor + delta;
            if (sf < 5) sf = 5;
            if (sf > 12) sf = 12;
            lora_cfg.spreading_factor = (uint8_t)sf;
            break;
        }
        case FIELD_BW: {
            int idx = bw_index() + delta;
            if (idx < 0) idx = 0;
            if (idx >= BW_COUNT) idx = BW_COUNT - 1;
            lora_cfg.bandwidth = BW_OPTIONS[idx];
            break;
        }
        case FIELD_CR: {
            int cr = (int)lora_cfg.coding_rate + delta;
            if (cr < 5) cr = 5;
            if (cr > 8) cr = 8;
            lora_cfg.coding_rate = (uint8_t)cr;
            break;
        }
        case FIELD_POWER: {
            int p = (int)lora_cfg.power + delta;
            if (p < 2) p = 2;
            if (p > 22) p = 22;
            lora_cfg.power = (uint8_t)p;
            break;
        }
        case FIELD_SYNC:
            lora_cfg.sync_word = (uint8_t)((lora_cfg.sync_word + delta) & 0xFF);
            break;
        case FIELD_PREAMBLE: {
            int pre = (int)lora_cfg.preamble_length + delta;
            if (pre < 2) pre = 2;
            if (pre > 65535) pre = 65535;
            lora_cfg.preamble_length = (uint16_t)pre;
            break;
        }
        case FIELD_FLOOD_ADVERT_INT:
        case FIELD_DIRECT_ADVERT_INT: {
            // 0 = off (default), then a ladder up to 24h. 12h matches the
            // MeshCore upstream-default flood interval that Renze recommends.
            // Capped at 12h (43200 s) -- fits uint16_t. Add a uint32_t
            // variant if a 24h+ preset is ever wanted.
            static const uint16_t presets[] = {0, 30, 60, 300, 900, 1800, 3600, 7200, 21600, 43200};
            const int             n         = (int)(sizeof(presets) / sizeof(presets[0]));
            uint16_t*             target =
                (selected == FIELD_FLOOD_ADVERT_INT) ? &flood_advert_interval_s : &direct_advert_interval_s;
            int idx = 0;  // default to off
            for (int i = 0; i < n; i++)
                if (presets[i] == *target) {
                    idx = i;
                    break;
                }
            idx     = ((idx + delta) % n + n) % n;
            *target = presets[idx];
            break;
        }
        case FIELD_PRESET: {
            int idx = lora_preset_match();
            if (idx < 0) {
                idx = (delta > 0) ? 0 : (LORA_PRESET_COUNT - 1);
            } else {
                idx = ((idx + delta) % LORA_PRESET_COUNT + LORA_PRESET_COUNT) % LORA_PRESET_COUNT;
            }
            lora_cfg.spreading_factor = LORA_PRESETS[idx].sf;
            lora_cfg.bandwidth        = LORA_PRESETS[idx].bw;
            lora_cfg.coding_rate      = LORA_PRESETS[idx].cr;
            break;
        }
        case FIELD_ROLE: {
            static const meshcore_device_role_t ROLES[] = {
                MESHCORE_DEVICE_ROLE_CHAT_NODE,
                MESHCORE_DEVICE_ROLE_REPEATER,
                MESHCORE_DEVICE_ROLE_ROOM_SERVER,
                MESHCORE_DEVICE_ROLE_SENSOR,
            };
            const int n   = (int)(sizeof(ROLES) / sizeof(ROLES[0]));
            int       idx = 0;
            for (int i = 0; i < n; i++)
                if (ROLES[i] == lora_role) {
                    idx = i;
                    break;
                }
            idx       = ((idx + delta) % n + n) % n;
            lora_role = ROLES[idx];
            break;
        }
        case FIELD_PATH_HASH_SIZE: {
            int v = (int)path_hash_size + delta;
            if (v < 1) v = 3;
            if (v > 3) v = 1;
            path_hash_size = (uint8_t)v;
            break;
        }
        case FIELD_SENSITIVITY:
            lora_cfg.rx_boost = !lora_cfg.rx_boost;  // toggle (delta direction irrelevant)
            break;
        case FIELD_COUNTRY: {
            int idx = 0;
            for (int i = 0; i < REGION_COUNTRY_COUNT; i++) {
                if (strcmp(REGION_COUNTRIES[i].country_code, country_code) == 0) {
                    idx = i;
                    break;
                }
            }
            idx = ((idx + delta) % REGION_COUNTRY_COUNT + REGION_COUNTRY_COUNT) % REGION_COUNTRY_COUNT;
            strncpy(country_code, REGION_COUNTRIES[idx].country_code, sizeof(country_code) - 1);
            country_code[sizeof(country_code) - 1] = '\0';
            break;
        }
        case FIELD_ANTENNA_GAIN: {
            if (country_code[0] == '-' || country_code[0] == '\0') break;  // locked
            int v = (int)antenna_gain_dbi + delta;
            if (v < -3) v = -3;
            if (v > 15) v = 15;
            antenna_gain_dbi = (int8_t)v;
            break;
        }
        case FIELD_WIFI_ENABLED:
            wifi_enabled = !wifi_enabled;
            break;
        case FIELD_WIFI_NETWORK: {
            int n = wifi_slots_count();
            if (n == 0) break;
            int pos = 0;
            for (int i = 0; i < n; i++) {
                if (wifi_slot_idx_at(i) == wifi_slot) {
                    pos = i;
                    break;
                }
            }
            pos       = ((pos + delta) % n + n) % n;
            wifi_slot = wifi_slot_idx_at(pos);
            break;
        }
        case FIELD_DISPLAY_BL:
        case FIELD_KB_BL:
        case FIELD_LED_BR: {
            // Backlight + LED brightness all cycle through the same 6 stops so
            // the slider feel is identical across the three fields.
            static const uint8_t stops[] = {5, 10, 25, 50, 75, 100};
            const int            n       = (int)(sizeof(stops) / sizeof(stops[0]));
            uint8_t*             v       = (field == FIELD_DISPLAY_BL) ? &display_brightness
                                           : (field == FIELD_KB_BL)    ? &keyboard_brightness
                                                                       : &led_brightness;
            int                  idx     = 0;
            for (int i = 0; i < n; i++)
                if (stops[i] == *v) {
                    idx = i;
                    break;
                }
            idx = ((idx + delta) % n + n) % n;
            *v  = stops[idx];
            // Apply live so the user sees the brightness change while scrolling.
            apply_brightness();
            break;
        }
        case FIELD_BLANK_AFTER: {
            // Idle-blank timeout cycler. 0 = off; otherwise seconds.
            static const uint16_t stops[] = {0, 30, 60, 300, 600, 1800};
            const int             n       = (int)(sizeof(stops) / sizeof(stops[0]));
            int                   idx     = 0;
            for (int i = 0; i < n; i++)
                if (stops[i] == display_blank_after_s) {
                    idx = i;
                    break;
                }
            idx                   = ((idx + delta) % n + n) % n;
            display_blank_after_s = stops[idx];
            break;
        }
        case FIELD_SOUND_VOLUME: {
            // Notification volume in 10% steps.
            static const uint8_t stops[] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
            const int            n       = (int)(sizeof(stops) / sizeof(stops[0]));
            int                  idx     = 0;
            for (int i = 0; i < n; i++)
                if (stops[i] == sound_volume_pct) {
                    idx = i;
                    break;
                }
            idx              = ((idx + delta) % n + n) % n;
            sound_volume_pct = stops[idx];
            sounds_apply_volume();
            break;
        }
        case FIELD_SOUND_DM:
        case FIELD_SOUND_CHANNEL:
        case FIELD_SOUND_ERROR:
        case FIELD_SOUND_BOOT: {
            // 0=off, 1..sounds_count() = index into /sd/meshcore/sounds/.
            uint8_t* slot  = (field == FIELD_SOUND_DM)        ? &sound_dm_slot
                             : (field == FIELD_SOUND_CHANNEL) ? &sound_channel_slot
                             : (field == FIELD_SOUND_ERROR)   ? &sound_error_slot
                                                              : &sound_boot_slot;
            // Off + one row per discovered WAV, capped at SOUNDS_MAX_SLOTS.
            int      avail = sounds_count();
            if (avail > SOUNDS_MAX_SLOTS) avail = SOUNDS_MAX_SLOTS;
            const int n   = avail + 1;  // include "Off"
            int       idx = (int)*slot;
            idx           = ((idx + delta) % n + n) % n;
            *slot         = (uint8_t)idx;
            break;
        }
        case FIELD_GPS_PROFILE: {
            int       idx = (int)gps_profile;
            const int n   = (int)GPS_PROFILE_COUNT;
            idx           = ((idx + delta) % n + n) % n;
            gps_profile   = (gps_profile_t)idx;
            break;
        }
        case FIELD_GPS_INTERVAL_S: {
            // Stops: 0 (Auto/profile default), 1, 2, 5, 10, 15, 30, 60, 120 s
            static const uint16_t stops[] = {0, 1, 2, 5, 10, 15, 30, 60, 120};
            const int             n       = sizeof(stops) / sizeof(stops[0]);
            int                   idx     = 0;
            for (int i = 0; i < n; i++)
                if (stops[i] == gps_custom_interval_s) {
                    idx = i;
                    break;
                }
            idx                   = ((idx + delta) % n + n) % n;
            gps_custom_interval_s = stops[idx];
            break;
        }
        case FIELD_GPS_DISTANCE_M: {
            // Stops: 0 (Auto/profile default), 5, 10, 25, 50, 100, 250, 500 m
            static const uint16_t stops[] = {0, 5, 10, 25, 50, 100, 250, 500};
            const int             n       = sizeof(stops) / sizeof(stops[0]);
            int                   idx     = 0;
            for (int i = 0; i < n; i++)
                if (stops[i] == gps_custom_distance_m) {
                    idx = i;
                    break;
                }
            idx                   = ((idx + delta) % n + n) % n;
            gps_custom_distance_m = stops[idx];
            break;
        }
        case FIELD_MAP_PROFILE:
            // Cycle only the enabled styles (Carto-only on shipping SD, so this
            // is a no-op until more are turned on in map.c).
            map_profile_set(map_profile_cycle(map_profile, delta));
            break;
        default:
            break;
    }
    dirty = true;
}

// Trigger a synchronous GPS read via the QWIIC-attached PA1010D and store the
// resulting lat/lon in NVS-backed settings. Blocks the UI for up to ~30s — we
// paint a "Searching" toast first so the user knows we're not frozen. Reports
// satellite count even on no-fix so the user can tell whether the chip is
// receiving anything at all (vs disconnected) or just needs more sky-view.
// ── Settings: text-field edit helpers (shared between handle_nav & handle_key)
static void settings_begin_text_edit(field_t f) {
    const char* src        = "";
    char        numbuf[24] = {0};
    if (f == FIELD_OWNER && owner_name[0] && owner_name[0] != '(')
        src = owner_name;
    else if (f == FIELD_ADV_NAME && lora_advert_name[0])
        src = lora_advert_name;
    else if (f == FIELD_REGION_SCOPE && region_scope[0])
        src = region_scope;
    else if (f == FIELD_GPS_LAT) {
        if (gps_position_valid) snprintf(numbuf, sizeof(numbuf), "%.6f", (double)gps_lat_e6 / 1e6);
        src = numbuf;
    } else if (f == FIELD_GPS_LON) {
        if (gps_position_valid) snprintf(numbuf, sizeof(numbuf), "%.6f", (double)gps_lon_e6 / 1e6);
        src = numbuf;
    } else if (f == FIELD_BLE_PIN) {
        // Prefill with the current 6-digit code (leading zeros included) so the
        // user edits in place rather than retyping from scratch.
        snprintf(numbuf, sizeof(numbuf), "%06lu", (unsigned long)ble_pin);
        src = numbuf;
    }
    strncpy(field_edit_buf, src, sizeof(field_edit_buf) - 1);
    field_edit_buf[sizeof(field_edit_buf) - 1] = '\0';
    field_edit_len                             = (int)strlen(field_edit_buf);
    field_editing_text                         = true;
}
static void settings_commit_text_edit(field_t f) {
    if (f == FIELD_OWNER) {
        strncpy(owner_name, field_edit_buf, sizeof(owner_name) - 1);
        owner_name[sizeof(owner_name) - 1] = '\0';
        save_owner_name();
    } else if (f == FIELD_ADV_NAME) {
        strncpy(lora_advert_name, field_edit_buf, sizeof(lora_advert_name) - 1);
        lora_advert_name[sizeof(lora_advert_name) - 1] = '\0';
        save_lora_advert_name();
    } else if (f == FIELD_REGION_SCOPE) {
        strncpy(region_scope, field_edit_buf, sizeof(region_scope) - 1);
        region_scope[sizeof(region_scope) - 1] = '\0';
        save_region_scope();
    } else if (f == FIELD_GPS_LAT || f == FIELD_GPS_LON) {
        // Empty (or whitespace) clears both coords; otherwise parse as decimal
        // degrees. We don't want a half-set position so both keys live or die
        // together — a single-axis edit re-saves the other from current state.
        char* trim = field_edit_buf;
        while (*trim == ' ') trim++;
        if (*trim == '\0') {
            gps_position_valid = false;
            gps_lat_e6         = 0;
            gps_lon_e6         = 0;
        } else {
            // Accept comma as decimal separator too (Dutch keyboard habit).
            for (char* p = field_edit_buf; *p; p++)
                if (*p == ',') *p = '.';
            double  v    = atof(field_edit_buf);
            int32_t v_e6 = (int32_t)(v * 1e6);
            if (f == FIELD_GPS_LAT)
                gps_lat_e6 = v_e6;
            else
                gps_lon_e6 = v_e6;
            // Mark valid once at least one axis has been set non-zero, OR if
            // both keys already had values (allowing fine-tune of one axis).
            if (gps_lat_e6 != 0 || gps_lon_e6 != 0) {
                gps_position_valid = true;
                gps_last_source    = GPS_SRC_MANUAL;
            }
        }
        save_gps_coords();
    } else if (f == FIELD_BLE_PIN) {
        // Digits-only buffer (enforced on input); empty stays 0. strtoul clamps
        // safely and the explicit modulo keeps it inside 0..999999.
        unsigned long v = strtoul(field_edit_buf, NULL, 10);
        ble_pin         = (uint32_t)(v % 1000000ul);
        save_ble_pin();
    }
    field_editing_text = false;
}

// ── Per-view navigation handlers ────────────────────────────────────────────
// Each view handles UP/DOWN/LEFT/RIGHT/RETURN for itself. handle_nav below
// keeps the global concerns (modal overlays, ESC chain, F4 emoji-open) and
// dispatches the rest into the matching view function. ESC was already
// taken by the time these run, so they only see directional keys + RETURN.

static void nav_home(uint32_t key) {
    int cols = 3;  // mirrors HOME_TILE_COLS in render_home.c
    if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (home_cursor - cols >= 0) home_cursor -= cols;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        int total = home_tile_count();
        if (home_cursor + cols < total) home_cursor += cols;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
        if (home_cursor > 0) home_cursor--;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
        if (home_cursor < home_tile_count() - 1) home_cursor++;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        open_home_tile(home_cursor);
    }
}

static void nav_nodes(uint32_t key) {
    if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (node_cursor > 0) node_cursor--;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        int upper = node_count + contact_count - 1;
        if (upper < 0) upper = 0;
        if (node_cursor < upper) node_cursor++;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
            int           idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
            if (node_cursor < idx_count) {
                display_row_t* d = &rows_dl[node_cursor];
                if (d->node_idx >= 0) {
                    node_entry_t* n = &node_list[d->node_idx];
                    dm_select_target(n->pub_key, n->name);
                    contact_ensure(n->pub_key, n->name, (uint8_t)n->role);
                } else if (d->is_contact) {
                    contact_t* c = &contacts[d->contact_idx];
                    dm_select_target(c->pub_key, c->alias);
                }
            }
            xSemaphoreGive(node_mutex);
        }
        if (dm_target_set) {
            current_view   = VIEW_CHAT;
            dm_inbox_mode  = false;
            led_dm_pending = false;
            update_notification_led();
        }
    }
}

static void nav_chat(uint32_t key) {
    if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (dm_inbox_mode) {
            if (dm_inbox_cursor > 0) dm_inbox_cursor--;
        } else {
            if (chat_scroll > 0) chat_scroll--;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        if (dm_inbox_mode) {
            int upper = (dm_target_set ? 1 : 0) + contact_count - 1;
            if (upper < 0) upper = 0;
            if (dm_inbox_cursor < upper) dm_inbox_cursor++;
        } else {
            chat_scroll++;  // render clamps to newest
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        if (dm_inbox_mode && !chat_typing) {
            int  idx_map[MAX_CONTACTS + 1];
            int  idx_count     = 0;
            bool active_on_top = dm_target_set;
            if (active_on_top) idx_map[idx_count++] = -1;
            for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
                if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0) continue;
                idx_map[idx_count++] = i;
            }
            if (dm_inbox_cursor < idx_count) {
                int e = idx_map[dm_inbox_cursor];
                if (e >= 0 && xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    dm_select_target(contacts[e].pub_key, contacts[e].alias);
                    xSemaphoreGive(node_mutex);
                }
                dm_inbox_mode  = false;
                led_dm_pending = false;
                update_notification_led();
            }
            return;
        }
        if (chat_typing && chat_input_len > 0) {
            if (dm_target_set) {
                uint8_t ack_crc[4] = {0};
                send_dm_message(chat_input, dm_target_pub, ack_crc);
                chat_add_dm(chat_input, true, dm_target_pub);
                chat_arm_ack_dm(ack_crc);
                meshcore_device_role_t r = MESHCORE_DEVICE_ROLE_CHAT_NODE;
                if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    for (int ni = 0; ni < MAX_NODES; ni++) {
                        if (node_list[ni].active &&
                            memcmp(node_list[ni].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0) {
                            r = node_list[ni].role;
                            break;
                        }
                    }
                    // contacts[] is shared with the RX task; ensure under the lock.
                    contact_ensure(dm_target_pub, dm_target_name, (uint8_t)r);
                    xSemaphoreGive(node_mutex);
                }
            } else {
                chat_add_message("(geen DM-target — kies een node in Nodes-tab)", false);
            }
            chat_input_len = 0;
            chat_input[0]  = '\0';
            chat_typing    = false;
        } else if (chat_typing) {
            chat_typing = false;
        }
    }
}

// Defined below (near handle_nav); used here by the channel-list D-pad path.
static void channel_commit_add(void);
static void channel_wizard_reset(void);
static void channel_wizard_menu_select(void);

static void nav_channel(uint32_t key) {
    if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (channel_adding && channel_wiz_step == 0) {
            if (channel_wiz_cursor > 0) channel_wiz_cursor--;
        } else if (channel_list_mode && !channel_adding) {
            if (channel_list_cursor > 0) channel_list_cursor--;
        } else if (!channel_list_mode && !channel_adding) {
            if (ch_scroll > 0) ch_scroll--;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        if (channel_adding && channel_wiz_step == 0) {
            if (channel_wiz_cursor < 1) channel_wiz_cursor++;
        } else if (channel_list_mode && !channel_adding) {
            if (channel_list_cursor < channel_count - 1) channel_list_cursor++;
        } else if (!channel_list_mode && !channel_adding) {
            ch_scroll++;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        if (channel_adding) {
            if (channel_wiz_step == 0) {
                channel_wizard_menu_select();
            } else {
                channel_commit_add();
            }
            return;
        }
        if (channel_list_mode && !channel_adding) {
            if (channel_list_cursor >= 0 && channel_list_cursor < channel_count &&
                channels[channel_list_cursor].active) {
                ch_select_channel(channel_list_cursor);
                channel_list_mode = false;
            }
            return;
        }
        if ((current_view == VIEW_CHANNEL) && chat_typing && chat_input_len > 0) {
            uint8_t fp[4];
            bool    sent = send_chat_message(chat_input, fp);
            ch_add_message(chat_input, true);
            if (sent)
                ch_arm_relay(fp);
            else
                ch_mark_not_sent();
            chat_input_len = 0;
            chat_input[0]  = '\0';
            chat_typing    = false;
        } else if (chat_typing) {
            chat_typing = false;
        }
    }
}

static void nav_settings(uint32_t key) {
    if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (settings_category_list_mode) {
            int cols = 4;
            if (settings_category_cursor - cols >= 0) settings_category_cursor -= cols;
        } else if (!edit_mode) {
            int first, last;
            settings_category_bounds(settings_category_active, &first, &last);
            int n    = last - first + 1;
            selected = first + (selected - first - 1 + n) % n;
        } else if (!field_editing_text)
            field_adjust(selected, +1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        if (settings_category_list_mode) {
            int cols  = 4;
            int total = settings_visible_category_count();
            if (settings_category_cursor + cols < total) settings_category_cursor += cols;
        } else if (!edit_mode) {
            int first, last;
            settings_category_bounds(settings_category_active, &first, &last);
            int n    = last - first + 1;
            selected = first + (selected - first + 1) % n;
        } else if (!field_editing_text)
            field_adjust(selected, -1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
        if (settings_category_list_mode) {
            if (settings_category_cursor > 0) settings_category_cursor--;
        } else if (edit_mode && !field_editing_text)
            field_adjust(selected, -1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
        if (settings_category_list_mode) {
            if (settings_category_cursor < settings_visible_category_count() - 1) settings_category_cursor++;
        } else if (edit_mode && !field_editing_text)
            field_adjust(selected, +1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        if (settings_category_list_mode) {
            // Drill into the focused category: the grid cursor is in
            // visible-slot space, translate to the real s_categories
            // index before drilling. Clamp the field cursor to the
            // first field of the target category.
            int real = settings_visible_category_real_idx(settings_category_cursor);
            if (real < 0) real = 0;
            // External tiles (Toolbox) switch straight to a top-level view
            // instead of drilling into a field list.
            app_view_t ext_view;
            if (settings_category_is_external(real, &ext_view)) {
                current_view = ext_view;
                return;
            }
            settings_category_active    = real;
            settings_category_list_mode = false;
            int first, last;
            settings_category_bounds(settings_category_active, &first, &last);
            selected        = first;
            settings_scroll = 0;
            return;
        }
        // FIELD_RADIO_FW / FIELD_RADIO_FW_APP / FIELD_DUTY_CYCLE are read-only.
        // FIELD_ANTENNA_GAIN is read-only until country is set.
        bool gain_locked = (selected == FIELD_ANTENNA_GAIN && (country_code[0] == '-' || country_code[0] == '\0'));
        if (selected == FIELD_RADIO_FW || selected == FIELD_RADIO_FW_APP || selected == FIELD_DUTY_CYCLE ||
            selected == FIELD_GPS_SOURCE || selected == FIELD_WIFI_SSID || selected == FIELD_WIFI_STATUS ||
            selected == FIELD_HTTP_URL || selected == FIELD_HTTP_API_KEY || selected == FIELD_HTTPS_CERT_FP ||
            gain_locked) {
            // ignore (read-only rows)
        } else if (selected == FIELD_HTTP_KEY_REGEN) {
            regenerate_http_api_key();
            snprintf(toast_text, sizeof(toast_text), "API key regenerated");
            toast_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        } else if (selected == FIELD_HTTPS_CERT_REGEN) {
            esp_err_t rc = http_server_regen_cert();
            snprintf(toast_text, sizeof(toast_text),
                     rc == ESP_OK ? "Cert regen'd — reinstall iPhone profile" : "Cert regen FAILED (rc=%d)", rc);
            toast_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        } else if (selected == FIELD_HTTP_QR) {
            qr_overlay_mode   = QR_MODE_OWNTRACKS;
            qr_from_settings  = true;
            qr_overlay_active = true;
        } else if (selected == FIELD_SEND_FLOOD_NOW) {
            send_advert();
            snprintf(toast_text, sizeof(toast_text), "Flood advert sent");
            toast_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        } else if (selected == FIELD_SEND_DIRECT_NOW) {
            send_advert_direct();
            snprintf(toast_text, sizeof(toast_text), "Direct adverts queued (1-hop)");
            toast_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        } else if (selected == FIELD_SOUND_TEST_DM) {
            sounds_play_dm();
        } else if (selected == FIELD_SOUND_TEST_CHANNEL) {
            sounds_play_channel();
        } else if (selected == FIELD_SOUND_TEST_ERROR) {
            sounds_play_error();
        } else if (selected == FIELD_SOUND_TEST_BOOT) {
            sounds_play_boot();
        } else if (selected == FIELD_BLE_ENABLED) {
            // Toggle row: flip + save. Takes effect on next app start;
            // tearing NimBLE down cleanly mid-runtime is messy enough that
            // a relaunch is the simpler contract.
            ble_enabled = !ble_enabled;
            save_ble_enabled();
            snprintf(toast_text, sizeof(toast_text), "BLE %s on next start", ble_enabled ? "ON" : "OFF");
            toast_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        } else if (!edit_mode) {
            // Refresh the cached launcher-slot list right before letting
            // the user cycle through it, so newly added networks land.
            if (selected == FIELD_WIFI_NETWORK) wifi_slots_refresh();
            edit_mode = true;
            if (selected == FIELD_OWNER || selected == FIELD_ADV_NAME || selected == FIELD_REGION_SCOPE ||
                selected == FIELD_GPS_LAT || selected == FIELD_GPS_LON || selected == FIELD_BLE_PIN) {
                settings_begin_text_edit(selected);
            }
        } else {
            if (field_editing_text)
                settings_commit_text_edit(selected);
            else
                field_save(selected);
            edit_mode = false;
            dirty     = false;
        }
    }
}

// ── Toolbox launcher + packet-log input ─────────────────────────────────────
static void open_toolbox_tile(void) {
    if (!toolbox_tile_enabled(toolbox_cursor)) return;
    app_view_t t = toolbox_tile_target(toolbox_cursor);
    if (t == VIEW_TOOLBOX_LOG) {
        toolbox_log_scroll = 0;
        toolbox_log_cursor = 0;
        toolbox_log_detail = false;
        toolbox_log_paused = false;
    } else if (t == VIEW_TOOLBOX_COVERAGE) {
        toolbox_coverage_cursor = 0;
        coverage_session_reset();  // fresh SD log + cleared results for this area test
    }
    current_view = t;
}

// Ping the repeater under the coverage cursor (3x, GPS-stamped). Re-collects so
// it indexes the same list the view rendered; no-op while a run is in flight.
static void coverage_ping_selected(void) {
    if (coverage_busy()) return;
    static coverage_repeater_t reps[COVERAGE_MAX_RESULTS];  // static: 64 entries is too large for the stack
    bool                       rv   = gps_live_valid || gps_position_valid;
    int32_t                    rlat = gps_live_valid ? gps_live_lat_e6 : gps_lat_e6;
    int32_t                    rlon = gps_live_valid ? gps_live_lon_e6 : gps_lon_e6;
    int n = coverage_collect_repeaters(reps, COVERAGE_MAX_RESULTS, rlat, rlon, rv, COVERAGE_RADIUS_M);
    if (toolbox_coverage_cursor < 0 || toolbox_coverage_cursor >= n) return;
    coverage_repeater_t* r = &reps[toolbox_coverage_cursor];
    coverage_ping_start(r->pub, r->name, gps_live_lat_e6, gps_live_lon_e6, gps_live_valid);
}

static void nav_toolbox_coverage(uint32_t key) {
    if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (toolbox_coverage_cursor > 0) toolbox_coverage_cursor--;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        toolbox_coverage_cursor++;  // render clamps to the repeater count
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        coverage_ping_selected();
    }
}

static void key_toolbox_coverage(char c) {
    if (c == 'w' || c == 'W') {
        if (toolbox_coverage_cursor > 0) toolbox_coverage_cursor--;
    } else if (c == 's' || c == 'S') {
        toolbox_coverage_cursor++;
    } else if (c == '\r' || c == '\n') {
        coverage_ping_selected();
    } else if (c == 'r' || c == 'R') {
        coverage_session_reset();
        toolbox_coverage_cursor = 0;
    }
}

static void nav_toolbox(uint32_t key) {
    int n = toolbox_tile_count();
    if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (toolbox_cursor > 0) toolbox_cursor--;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        if (toolbox_cursor < n - 1) toolbox_cursor++;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        open_toolbox_tile();
    }
}

static void nav_toolbox_log(uint32_t key) {
    if (toolbox_log_detail) return;  // detail view is read-only; ESC closes it
    if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (toolbox_log_cursor > 0) toolbox_log_cursor--;
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        toolbox_log_cursor++;  // render clamps to the available range
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        toolbox_log_detail = true;  // open the full breakdown of the selected entry
    }
}

static void key_toolbox(char c) {
    if (c == 'w' || c == 'W') {
        if (toolbox_cursor > 0) toolbox_cursor--;
    } else if (c == 's' || c == 'S') {
        if (toolbox_cursor < toolbox_tile_count() - 1) toolbox_cursor++;
    } else if (c == '\r' || c == '\n') {
        open_toolbox_tile();
    }
}

static void key_toolbox_log(char c) {
    if (toolbox_log_detail) return;  // read-only; ESC closes it
    if (c == 'w' || c == 'W') {
        if (toolbox_log_cursor > 0) toolbox_log_cursor--;
    } else if (c == 's' || c == 'S') {
        toolbox_log_cursor++;
    } else if (c == '\r' || c == '\n') {
        toolbox_log_detail = true;
    } else if (c == 'h' || c == 'H') {
        toolbox_log_dissect = !toolbox_log_dissect;
    } else if (c == 'p' || c == 'P') {
        toolbox_log_paused = !toolbox_log_paused;
    } else if (c == 'c' || c == 'C') {
        diag_clear();
        toolbox_log_cursor = 0;
        toolbox_log_scroll = 0;
    } else if (c == 'e' || c == 'E') {
        // Export the ring to /sd/meshcore/log/pkt_<unix>.csv (S is the scroll
        // key in this view, so the SD dump lives on E for "Export").
        toolbox_log_export_sd();
    }
}

// Build a channel name from a typed slice, auto-prefixing '#' (the MeshCore
// convention for non-Public channels) and clamping to CHANNEL_NAME_MAX_LEN.
static void build_channel_name(char* out, const char* in) {
    out[0]          = '\0';
    bool needs_hash = (in[0] != '#');
    int  cap        = CHANNEL_NAME_MAX_LEN - (needs_hash ? 1 : 0);
    if (needs_hash) {
        out[0] = '#';
        out[1] = '\0';
    }
    strncat(out, in, cap);
}

// Reset the add/create wizard back to the channel list (cancel or finish).
static void channel_wizard_reset(void) {
    channel_adding      = false;
    channel_creating    = false;
    channel_wiz_step    = 0;
    channel_wiz_private = false;
    field_editing_text  = false;
    field_edit_len      = 0;
    field_edit_buf[0]   = '\0';
    channel_wiz_name[0] = '\0';
}

// Menu (step 0) -> name (step 1): record the #community/private choice.
static void channel_wizard_menu_select(void) {
    channel_wiz_private = (channel_wiz_cursor == 1);
    channel_wiz_step    = 1;
    field_editing_text  = true;
    field_edit_len      = 0;
    field_edit_buf[0]   = '\0';
}

// Commit the current wizard step (shared by the D-pad and keyboard paths).
//  step 1 (name): #community -> add now; private+create -> mint key + share QR;
//                 private+add -> stash the name, advance to the secret step.
//  step 2 (secret): parse 32 hex -> add the private channel with that key.
static void channel_commit_add(void) {
    if (field_edit_len == 0) return;  // empty entry: stay on this step

    if (channel_wiz_step == 1) {
        if (!channel_wiz_private) {
            char nm[CHANNEL_NAME_MAX_LEN + 1];
            build_channel_name(nm, field_edit_buf);  // '#'-prefixed, key = SHA256
            int slot = channels_add_by_name(nm);
            if (slot > 0) channel_list_cursor = slot;
            channel_wizard_reset();
            return;
        }
        if (channel_creating) {
            char nm[CHANNEL_NAME_MAX_LEN + 1];
            strncpy(nm, field_edit_buf, CHANNEL_NAME_MAX_LEN);  // private: name as typed
            nm[CHANNEL_NAME_MAX_LEN] = '\0';
            int slot                 = channels_create_private(nm);
            if (slot > 0) {
                channel_list_cursor = slot;
                qr_channel_idx      = slot;  // jump straight to its share QR
                qr_overlay_mode     = QR_MODE_CHANNEL;
                qr_from_channel     = true;
                qr_overlay_active   = true;
            }
            channel_wizard_reset();
            return;
        }
        // private "add": hold the name, ask for the secret next.
        strncpy(channel_wiz_name, field_edit_buf, CHANNEL_NAME_MAX_LEN);
        channel_wiz_name[CHANNEL_NAME_MAX_LEN] = '\0';
        channel_wiz_step                       = 2;
        field_edit_len                         = 0;
        field_edit_buf[0]                      = '\0';
        return;
    }

    if (channel_wiz_step == 2) {
        uint8_t secret[CHANNEL_SECRET_LEN];
        char    ignore[CHANNEL_NAME_MAX_LEN + 1];
        if (channel_parse_share(field_edit_buf, ignore, sizeof(ignore), secret)) {
            int slot = channels_add_with_secret(channel_wiz_name[0] ? channel_wiz_name : "private", secret);
            if (slot > 0) channel_list_cursor = slot;
            channel_wizard_reset();
        } else {
            field_edit_len    = 0;  // invalid 32-hex: clear, stay on the secret step
            field_edit_buf[0] = '\0';
        }
        return;
    }
}

void handle_nav(uint32_t key) {
    if (qr_overlay_active) {
        // Overlay swallows all nav keys; the red X (F1) dismisses it. ESC is no
        // longer a back key in submenus.
        if (key == BSP_INPUT_NAVIGATION_KEY_F1) {
            qr_overlay_active = false;
            qr_overlay_mode   = QR_MODE_CONTACT;
            qr_channel_idx    = -1;
            if (qr_from_home) {
                qr_from_home = false;
                current_view = VIEW_HOME;
            } else if (qr_from_settings) {
                qr_from_settings = false;
            } else if (qr_from_channel) {
                qr_from_channel   = false;
                current_view      = VIEW_CHANNEL;
                channel_list_mode = true;  // land back on the channel list
            }
        }
        return;
    }

    // Emoji picker (F4 button = green circle on Tanmatsu) opens during chat typing.
    if (key == BSP_INPUT_NAVIGATION_KEY_F4 && chat_typing &&
        (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL) && !emoji_picker_active) {
        emoji_picker_active = true;
        emoji_picker_cursor = 0;
        return;
    }

    // Emoji picker navigation via D-pad / RETURN / ESC. Without this branch
    // selecting on Tanmatsu's D-pad (which fires RETURN as nav-event, not as
    // '\r' in handle_key) wouldn't close the picker — leaving it hanging open
    // for the next chat-typing session.
    if (emoji_picker_active && chat_typing && (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL)) {
        const int cols = 4;
        // Red X (F1) closes; F4 (the green circle) toggles — pressing it again
        // closes the picker instead of leaving it stuck open (which blocked
        // leaving chat). ESC is no longer a back key in submenus.
        if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_F4) {
            emoji_picker_active = false;
            return;
        }
        if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
            int idx = emoji_picker_cursor;
            if (idx >= 0 && idx < EMOJI_COUNT) {
                const emoji_entry_t* e = &EMOJI_SET[idx];
                if (chat_input_len + e->utf8_len <= MAX_INPUT_LEN) {
                    memcpy(&chat_input[chat_input_len], e->utf8, e->utf8_len);
                    chat_input_len             += e->utf8_len;
                    chat_input[chat_input_len]  = '\0';
                }
            }
            emoji_picker_active = false;
            return;
        }
        if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
            if (emoji_picker_cursor > 0) emoji_picker_cursor--;
            return;
        }
        if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
            if (emoji_picker_cursor < EMOJI_COUNT - 1) emoji_picker_cursor++;
            return;
        }
        if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
            if (emoji_picker_cursor - cols >= 0) emoji_picker_cursor -= cols;
            return;
        }
        if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
            if (emoji_picker_cursor + cols < EMOJI_COUNT) emoji_picker_cursor += cols;
            return;
        }
    }

    if (key == BSP_INPUT_NAVIGATION_KEY_ESC) {
        // ESC exits the app, but only from the home tile-grid. In every submenu
        // it does nothing — the red X (F1) is the single back/cancel key, so
        // back-navigation can never quit the app by accident.
        if (current_view == VIEW_HOME) {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_F1) {
        if (edit_mode) {
            edit_mode          = false;
            field_editing_text = false;
            dirty              = false;
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
            load_gps_coords();
            load_lora_config();
        } else if (chat_typing) {
            // The red X cancels typing and clears the buffer (the Tanmatsu's
            // red X only fires a nav-event, so this can't be deferred to
            // handle_key).
            chat_typing    = false;
            chat_input_len = 0;
            chat_input[0]  = '\0';
        } else if (current_view == VIEW_CHAT && !dm_inbox_mode) {
            dm_inbox_mode = true;
        } else if (current_view == VIEW_CHANNEL && channel_adding) {
            channel_wizard_reset();
        } else if (current_view == VIEW_CHANNEL && !channel_list_mode) {
            channel_list_mode = true;
        } else if (current_view == VIEW_SETTINGS && !settings_category_list_mode) {
            // First ESC out of a drilled-in settings category returns to the
            // category list; second ESC then falls through to HOME.
            settings_category_list_mode = true;
            settings_scroll             = 0;
        } else if (current_view == VIEW_TOOLBOX_LOG && toolbox_log_detail) {
            toolbox_log_detail = false;  // first ESC closes the detail breakdown
        } else if (current_view == VIEW_TOOLBOX_LOG || current_view == VIEW_TOOLBOX_COVERAGE) {
            current_view = VIEW_TOOLBOX;  // back to the launcher
        } else if (current_view == VIEW_TOOLBOX) {
            // Toolbox was reached from the Settings grid — return there.
            current_view                = VIEW_SETTINGS;
            settings_category_list_mode = true;
        } else if (current_view != VIEW_HOME) {
            // The red X returns to the home tile-grid and stops there; mashing
            // it can never exit the app. ESC (handled above) is the only exit.
            current_view = VIEW_HOME;
        }
        // At home the red X does nothing — ESC is the exit key.
    } else {
        // Direction / RETURN: dispatch to the active view's handler.
        switch (current_view) {
            case VIEW_HOME:
                nav_home(key);
                break;
            case VIEW_SETTINGS:
                nav_settings(key);
                break;
            case VIEW_NODES:
                nav_nodes(key);
                break;
            case VIEW_CHAT:
                nav_chat(key);
                break;
            case VIEW_CHANNEL:
                nav_channel(key);
                break;
            case VIEW_MAP:
                // Arrow-key pan. 1/4 tile per press matches a comfortable
                // step at zoom 8–10 (≈ 64 px on screen).
                if (key == BSP_INPUT_NAVIGATION_KEY_UP)
                    map_state_pan(0, -1);
                else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN)
                    map_state_pan(0, 1);
                else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT)
                    map_state_pan(-1, 0);
                else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT)
                    map_state_pan(1, 0);
                break;
            case VIEW_TOOLBOX:
                nav_toolbox(key);
                break;
            case VIEW_TOOLBOX_LOG:
                nav_toolbox_log(key);
                break;
            case VIEW_TOOLBOX_COVERAGE:
                nav_toolbox_coverage(key);
                break;
            default:
                break;
        }
    }
}

// ── handle_key per-view dispatch helpers ────────────────────────────────────
// Mode intercepts (QR overlay, settings text-edit, DM-inbox, channel-list,
// chat-typing) and the global Tab/ESC handlers stay in handle_key itself.
// Everything below is the post-intercept key fan-out: W/S/A/D/F/L/Q/Enter/R/
// `<>,.`/D, previously a long `else if (current_view == VIEW_X)` cascade.

static void key_home(char c) {
    const int cols  = 3;  // mirrors HOME_TILE_COLS in render_home.c
    const int total = home_tile_count();
    if (c == 'w' || c == 'W') {
        if (home_cursor - cols >= 0) home_cursor -= cols;
    } else if (c == 's' || c == 'S') {
        if (home_cursor + cols < total) home_cursor += cols;
    } else if (c == 'a' || c == 'A') {
        if (home_cursor > 0) home_cursor--;
    } else if (c == 'd' || c == 'D') {
        if (home_cursor < total - 1) home_cursor++;
    } else if (c == '\r' || c == '\n') {
        open_home_tile(home_cursor);
    }
}

static void key_nodes(char c) {
    if (c == 'w' || c == 'W') {
        if (node_cursor > 0) node_cursor--;
    } else if (c == 's' || c == 'S') {
        int upper = node_count + contact_count - 1;
        if (upper < 0) upper = 0;
        if (node_cursor < upper) node_cursor++;
    } else if (c == 'a' || c == 'A') {
        send_advert();
    } else if (c == 'f' || c == 'F') {
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
            int           idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
            if (node_cursor < idx_count) {
                display_row_t* d = &rows_dl[node_cursor];
                if (d->is_contact) {
                    contact_toggle(contacts[d->contact_idx].pub_key, NULL, 0);
                } else if (d->node_idx >= 0) {
                    node_entry_t* n = &node_list[d->node_idx];
                    int           r = contact_toggle(n->pub_key, n->name, (uint8_t)n->role);
                    if (r < 0) ESP_LOGW(TAG, "Contacts list is full (%d/%d)", contact_count, MAX_CONTACTS);
                }
            }
            xSemaphoreGive(node_mutex);
        }
    } else if (c == 'l' || c == 'L') {
        static const meshcore_device_role_t cycle[] = {
            MESHCORE_DEVICE_ROLE_UNKNOWN,     MESHCORE_DEVICE_ROLE_CHAT_NODE, MESHCORE_DEVICE_ROLE_REPEATER,
            MESHCORE_DEVICE_ROLE_ROOM_SERVER, MESHCORE_DEVICE_ROLE_SENSOR,
        };
        const int n   = (int)(sizeof(cycle) / sizeof(cycle[0]));
        int       idx = 0;
        for (int i = 0; i < n; i++)
            if (cycle[i] == node_filter) {
                idx = i;
                break;
            }
        node_filter = cycle[(idx + 1) % n];
        node_scroll = 0;
        node_cursor = 0;
    } else if ((c == 'q' || c == 'Q') && identity_is_ready()) {
        qr_overlay_active = true;
    } else if (c == '\r' || c == '\n') {
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
            int           idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
            if (node_cursor < idx_count) {
                display_row_t* d = &rows_dl[node_cursor];
                if (d->node_idx >= 0) {
                    node_entry_t* n = &node_list[d->node_idx];
                    dm_select_target(n->pub_key, n->name);
                    contact_ensure(n->pub_key, n->name, (uint8_t)n->role);
                } else if (d->is_contact) {
                    contact_t* c2 = &contacts[d->contact_idx];
                    dm_select_target(c2->pub_key, c2->alias);
                }
            }
            xSemaphoreGive(node_mutex);
        }
        if (dm_target_set) {
            current_view   = VIEW_CHAT;
            dm_inbox_mode  = false;
            led_dm_pending = false;
            update_notification_led();
        }
    }
}

static void key_settings(char c) {
    if (c == 'w' || c == 'W') {
        if (settings_category_list_mode) {
            int cols = 4;
            if (settings_category_cursor - cols >= 0) settings_category_cursor -= cols;
        } else if (!edit_mode) {
            int first, last;
            settings_category_bounds(settings_category_active, &first, &last);
            int n    = last - first + 1;
            selected = first + (selected - first - 1 + n) % n;
        } else if (!field_editing_text)
            field_adjust(selected, +1);
    } else if (c == 's' || c == 'S') {
        if (settings_category_list_mode) {
            int cols  = 4;
            int total = settings_visible_category_count();
            if (settings_category_cursor + cols < total) settings_category_cursor += cols;
        } else if (!edit_mode) {
            int first, last;
            settings_category_bounds(settings_category_active, &first, &last);
            int n    = last - first + 1;
            selected = first + (selected - first + 1) % n;
        } else if (!field_editing_text)
            field_adjust(selected, -1);
    } else if ((c == 'a' || c == 'A') && settings_category_list_mode) {
        if (settings_category_cursor > 0) settings_category_cursor--;
    } else if ((c == 'd' || c == 'D') && settings_category_list_mode) {
        if (settings_category_cursor < settings_visible_category_count() - 1) settings_category_cursor++;
    } else if (c == '<' || c == ',') {
        if (edit_mode && !field_editing_text) field_adjust(selected, -1);
    } else if (c == '>' || c == '.') {
        if (edit_mode && !field_editing_text) field_adjust(selected, +1);
    } else if (c == '\r' || c == '\n') {
        if (settings_category_list_mode) {
            // The grid cursor is in visible-slot space (Advert is hidden);
            // translate to the real s_categories index, exactly like the D-pad
            // RETURN path in nav_settings. Assigning the slot directly opened
            // the wrong category for every slot at or after a hidden one.
            int real = settings_visible_category_real_idx(settings_category_cursor);
            if (real < 0) real = 0;
            // External tiles (Toolbox) switch straight to a top-level view.
            app_view_t ext_view;
            if (settings_category_is_external(real, &ext_view)) {
                current_view = ext_view;
                return;
            }
            settings_category_active    = real;
            settings_category_list_mode = false;
            int first, last;
            settings_category_bounds(settings_category_active, &first, &last);
            selected        = first;
            settings_scroll = 0;
            return;
        }
        // FIELD_RADIO_FW / FIELD_RADIO_FW_APP / FIELD_DUTY_CYCLE are read-only — Enter no-op.
        // FIELD_ANTENNA_GAIN is read-only until country is set (otherwise gain has no effect).
        bool gain_locked = (selected == FIELD_ANTENNA_GAIN && (country_code[0] == '-' || country_code[0] == '\0'));
        if (selected == FIELD_RADIO_FW || selected == FIELD_RADIO_FW_APP || selected == FIELD_DUTY_CYCLE ||
            selected == FIELD_GPS_SOURCE || selected == FIELD_WIFI_SSID || selected == FIELD_WIFI_STATUS ||
            selected == FIELD_HTTP_URL || selected == FIELD_HTTP_API_KEY || selected == FIELD_HTTPS_CERT_FP ||
            gain_locked) {
            // ignore (read-only rows)
        } else if (selected == FIELD_HTTP_KEY_REGEN) {
            regenerate_http_api_key();
            snprintf(toast_text, sizeof(toast_text), "API key regenerated");
            toast_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        } else if (selected == FIELD_HTTPS_CERT_REGEN) {
            esp_err_t rc = http_server_regen_cert();
            snprintf(toast_text, sizeof(toast_text),
                     rc == ESP_OK ? "Cert regen'd — reinstall iPhone profile" : "Cert regen FAILED (rc=%d)", rc);
            toast_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        } else if (selected == FIELD_HTTP_QR) {
            qr_overlay_mode   = QR_MODE_OWNTRACKS;
            qr_from_settings  = true;
            qr_overlay_active = true;
        } else if (selected == FIELD_SEND_FLOOD_NOW) {
            send_advert();
            snprintf(toast_text, sizeof(toast_text), "Flood advert sent");
            toast_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        } else if (selected == FIELD_SEND_DIRECT_NOW) {
            send_advert_direct();
            snprintf(toast_text, sizeof(toast_text), "Direct adverts queued (1-hop)");
            toast_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        } else if (selected == FIELD_SOUND_TEST_DM) {
            sounds_play_dm();
        } else if (selected == FIELD_SOUND_TEST_CHANNEL) {
            sounds_play_channel();
        } else if (selected == FIELD_SOUND_TEST_ERROR) {
            sounds_play_error();
        } else if (selected == FIELD_SOUND_TEST_BOOT) {
            sounds_play_boot();
        } else if (selected == FIELD_BLE_ENABLED) {
            ble_enabled = !ble_enabled;
            save_ble_enabled();
            snprintf(toast_text, sizeof(toast_text), "BLE %s on next start", ble_enabled ? "ON" : "OFF");
            toast_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        } else if (!edit_mode) {
            if (selected == FIELD_WIFI_NETWORK) wifi_slots_refresh();
            edit_mode = true;
            if (selected == FIELD_OWNER || selected == FIELD_ADV_NAME || selected == FIELD_REGION_SCOPE ||
                selected == FIELD_GPS_LAT || selected == FIELD_GPS_LON || selected == FIELD_BLE_PIN) {
                settings_begin_text_edit(selected);
            }
        } else {
            if (field_editing_text)
                settings_commit_text_edit(selected);
            else
                field_save(selected);
            edit_mode = false;
            dirty     = false;
        }
    } else if (c == 'r' || c == 'R') {
        wifi_slots_refresh();
        load_owner_name();
        load_lora_advert_name();
        load_region_scope();
        load_gps_coords();
        load_lora_config();
        dirty              = false;
        edit_mode          = false;
        field_editing_text = false;
    }
}

static void key_chat(char c) {
    // Only the inbox-mode 'D' delete falls through to here; W/S/T are handled
    // by the dm_inbox intercept above, and chat_typing keys by the chat/channel
    // typing intercept.
    if (!((c == 'd' || c == 'D') && dm_inbox_mode && !chat_typing)) return;

    int  idx_map[MAX_CONTACTS + 1];
    int  idx_count     = 0;
    bool active_on_top = dm_target_set;
    if (active_on_top) idx_map[idx_count++] = -1;
    for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
        if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0) continue;
        idx_map[idx_count++] = i;
    }
    if (dm_inbox_cursor < idx_count) {
        int     e = idx_map[dm_inbox_cursor];
        uint8_t target_pub[MESHCORE_PUB_KEY_SIZE];
        char    target_name[MESHCORE_MAX_NAME_SIZE + 1];
        int     ci = -1;
        if (e == -1) {
            memcpy(target_pub, dm_target_pub, MESHCORE_PUB_KEY_SIZE);
            strncpy(target_name, dm_target_name, sizeof(target_name) - 1);
            target_name[sizeof(target_name) - 1] = '\0';
            dm_target_set                        = false;
            memset(chat_msgs, 0, sizeof(chat_msgs));
            chat_head = chat_count = chat_scroll = 0;
        }
        // contacts[] is shared with the RX task; resolve the slot + shift it out
        // under node_mutex. history_delete_dm does SD I/O, so it stays outside.
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (e == -1) {
                ci = contact_find(target_pub);
            } else {
                ci = e;
                memcpy(target_pub, contacts[ci].pub_key, MESHCORE_PUB_KEY_SIZE);
                strncpy(target_name, contacts[ci].alias, sizeof(target_name) - 1);
                target_name[sizeof(target_name) - 1] = '\0';
            }
            xSemaphoreGive(node_mutex);
        }

        history_delete_dm(target_pub);
        if (ci >= 0 && xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (int j = ci; j < contact_count - 1; j++) contacts[j] = contacts[j + 1];
            memset(&contacts[contact_count - 1], 0, sizeof(contact_t));
            contact_count--;
            contacts_save();
            xSemaphoreGive(node_mutex);
        }
        if (dm_inbox_cursor > 0) dm_inbox_cursor--;
        ESP_LOGI(TAG, "DM deleted by user (D): %s", target_name);
    }
}

// Map-view key handler. Pan/zoom logic is added in Phase 3; for now this is
// a stub so VIEW_MAP doesn't fall through to a sibling view's keymap.
static void key_map(char c);

static void key_channel(char c) {
    // 'R' clears the active channel's history (RAM + file). Channel list-mode
    // and chat-typing intercept above already handle their own keymaps.
    if (!((c == 'r' || c == 'R') && !chat_typing && !channel_list_mode)) return;

    if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memset(ch_msgs, 0, sizeof(ch_msgs));
        ch_head   = 0;
        ch_count  = 0;
        ch_scroll = 0;
        xSemaphoreGive(ch_mutex);
    }
    if (active_channel_idx >= 0 && active_channel_idx < channel_count && channels[active_channel_idx].active) {
        history_delete_channel(channels[active_channel_idx].secret);
        ESP_LOGI(TAG, "Channel history cleared by user (R): %s", channels[active_channel_idx].name);
    }
}

void handle_key(char c) {
    if (c == 27) {
        // Keyboard ESC exits only from the home tile-grid. In every submenu /
        // modal it does nothing — the physical red X (F1) is the single back
        // and cancel key, matching handle_nav so the two input paths agree.
        if (current_view == VIEW_HOME && !qr_overlay_active) {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
        return;
    }

    if (qr_overlay_active) {
        return;  // overlay swallows keyboard keys; the red X (F1) dismisses it
    }

    // Settings text-edit intercepts all printables so the global W/S/T/F/L/Q/R
    // shortcuts don't fire while typing.
    if (current_view == VIEW_SETTINGS && edit_mode && field_editing_text) {
        if (c == '\r' || c == '\n') {  // Enter: save (red X cancels, via handle_nav)
            settings_commit_text_edit(selected);
            edit_mode = false;
            dirty     = false;
        } else if (c == 127 || c == 8) {  // Backspace
            if (field_edit_len > 0) field_edit_buf[--field_edit_len] = '\0';
        } else if (c >= 32 && c < 127 && field_edit_len < (int)sizeof(field_edit_buf) - 1) {
            // Region scope: force lowercase, only [a-z 0-9 -] accepted.
            if (selected == FIELD_REGION_SCOPE) {
                if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
                bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
                if (!ok) return;
            }
            // BLE pairing code: digits only, capped at 6 characters.
            if (selected == FIELD_BLE_PIN) {
                if (c < '0' || c > '9' || field_edit_len >= 6) return;
            }
            field_edit_buf[field_edit_len++] = c;
            field_edit_buf[field_edit_len]   = '\0';
        }
        return;
    }

    // DM inbox view: own keymap (no typing here).
    if (current_view == VIEW_CHAT && dm_inbox_mode && !chat_typing) {
        if (c == 'w' || c == 'W') {
            if (dm_inbox_cursor > 0) dm_inbox_cursor--;
            return;
        }
        if (c == 's' || c == 'S') {
            int upper = (dm_target_set ? 1 : 0) + contact_count - 1;
            if (upper < 0) upper = 0;
            if (dm_inbox_cursor < upper) dm_inbox_cursor++;
            return;
        }
        // T to type only works inside the chat view; here we ignore it.
        if (c == 't' || c == 'T') return;
        // Tab falls through (ESC is handled at the top of handle_key).
    }

    // Channel list-mode: own keymap (W/S nav, Enter open, A add, D delete) plus
    // text-input intercept when channel_adding is active. Must come before the
    // chat-typing branch so chat_typing keys don't fire here.
    if (current_view == VIEW_CHANNEL && channel_list_mode) {
        if (channel_adding) {
            // Step 0 = pick #community/private; steps 1-2 = text entry (name, then
            // secret for a private "add"). Red X cancels (handle_nav).
            if (channel_wiz_step == 0) {
                if (c == 'w' || c == 'W') {
                    if (channel_wiz_cursor > 0) channel_wiz_cursor--;
                } else if (c == 's' || c == 'S') {
                    if (channel_wiz_cursor < 1) channel_wiz_cursor++;
                } else if (c == '\r' || c == '\n') {
                    channel_wizard_menu_select();
                }
                return;
            }
            if (c == '\r' || c == '\n') {
                channel_commit_add();
                return;
            }
            if (c == 127 || c == 8) {
                if (field_edit_len > 0) {
                    field_edit_buf[--field_edit_len] = '\0';
                }
                return;
            }
            if (c >= 32 && c < 127 && field_edit_len < (int)sizeof(field_edit_buf) - 1) {
                field_edit_buf[field_edit_len++] = c;
                field_edit_buf[field_edit_len]   = '\0';
                return;
            }
            return;  // swallow other keys while editing
        }

        if (c == 'w' || c == 'W') {
            if (channel_list_cursor > 0) channel_list_cursor--;
            return;
        }
        if (c == 's' || c == 'S') {
            if (channel_list_cursor < channel_count - 1) channel_list_cursor++;
            return;
        }
        if (c == 'a' || c == 'A') {
            channel_adding     = true;
            channel_creating   = false;
            channel_wiz_step   = 0;  // open the #community/private menu
            channel_wiz_cursor = 0;
            field_editing_text = false;
            return;
        }
        if (c == 'c' || c == 'C') {
            channel_adding     = true;
            channel_creating   = true;
            channel_wiz_step   = 0;  // open the #community/private menu
            channel_wiz_cursor = 0;
            field_editing_text = false;
            return;
        }
        if (c == 'q' || c == 'Q') {
            // Show the share QR (meshcore:// link) for the selected channel.
            if (channel_list_cursor >= 0 && channel_list_cursor < channel_count &&
                channels[channel_list_cursor].active) {
                qr_channel_idx    = channel_list_cursor;
                qr_overlay_mode   = QR_MODE_CHANNEL;
                qr_from_channel   = true;
                qr_overlay_active = true;
            }
            return;
        }
        if (c == 'd' || c == 'D') {
            // Public (idx 0) cannot be removed.
            if (channel_list_cursor > 0 && channel_list_cursor < channel_count) {
                int removed = channel_list_cursor;
                channels_remove(removed);
                if (channel_list_cursor >= channel_count) channel_list_cursor = channel_count - 1;
                if (channel_list_cursor < 0) channel_list_cursor = 0;
            }
            return;
        }
        if (c == '\r' || c == '\n') {
            if (channel_list_cursor >= 0 && channel_list_cursor < channel_count &&
                channels[channel_list_cursor].active) {
                ch_select_channel(channel_list_cursor);  // clears + reloads this channel's history
                channel_list_mode = false;
            }
            return;
        }
        // Tab falls through (ESC is handled at the top of handle_key).
    }

    // Chat / Channel view input — intercept everything when typing.
    if (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL) {
        // Emoji picker overlay swallows all keys when active.
        if (chat_typing && emoji_picker_active) {
            const int cols = 4;
            // Red X / F4 close the picker (handle_nav); keyboard ESC no longer does.
            if (c == '\r' || c == '\n') {
                int idx = emoji_picker_cursor;
                if (idx >= 0 && idx < EMOJI_COUNT) {
                    const emoji_entry_t* e = &EMOJI_SET[idx];
                    if (chat_input_len + e->utf8_len <= MAX_INPUT_LEN) {
                        memcpy(&chat_input[chat_input_len], e->utf8, e->utf8_len);
                        chat_input_len             += e->utf8_len;
                        chat_input[chat_input_len]  = '\0';
                    }
                }
                emoji_picker_active = false;
                return;
            }
            if (c == 'a' || c == 'A') {
                if (emoji_picker_cursor > 0) emoji_picker_cursor--;
                return;
            }
            if (c == 'd' || c == 'D') {
                if (emoji_picker_cursor < EMOJI_COUNT - 1) emoji_picker_cursor++;
                return;
            }
            if (c == 'w' || c == 'W') {
                if (emoji_picker_cursor - cols >= 0) emoji_picker_cursor -= cols;
                return;
            }
            if (c == 's' || c == 'S') {
                if (emoji_picker_cursor + cols < EMOJI_COUNT) emoji_picker_cursor += cols;
                return;
            }
            return;  // swallow the rest
        }
        if (chat_typing) {
            // Red X cancels typing (handle_nav); keyboard ESC no longer does.
            if (c == '\r' || c == '\n') {
                if (chat_input_len > 0) {
                    if (current_view == VIEW_CHANNEL) {
                        uint8_t fp[4];
                        bool    sent = send_chat_message(chat_input, fp);
                        ch_add_message(chat_input, true);
                        if (sent)
                            ch_arm_relay(fp);
                        else
                            ch_mark_not_sent();
                    } else if (dm_target_set) {
                        uint8_t ack_crc[4] = {0};
                        send_dm_message(chat_input, dm_target_pub, ack_crc);
                        chat_add_dm(chat_input, true, dm_target_pub);
                        chat_arm_ack_dm(ack_crc);
                    } else {
                        send_chat_message(chat_input, NULL);
                        chat_add_message(chat_input, true);
                    }
                    chat_input_len = 0;
                    chat_input[0]  = '\0';
                }
                chat_typing = false;
            } else if (c == 127 || c == 8) {
                if (chat_input_len > 0) {
                    chat_input[--chat_input_len] = '\0';
                }
            } else if (c >= 32 && c < 127 && chat_input_len < MAX_INPUT_LEN) {
                chat_input[chat_input_len++] = c;
                chat_input[chat_input_len]   = '\0';
            }
            return;
        } else {
            if (c == 't' || c == 'T') {
                if (current_view == VIEW_CHANNEL || (current_view == VIEW_CHAT && !dm_inbox_mode)) {
                    chat_typing = true;
                }
                return;
            }
            if (c == 'w' || c == 'W') {
                if (current_view == VIEW_CHANNEL) {
                    if (ch_scroll > 0) ch_scroll--;
                } else {
                    if (chat_scroll > 0) chat_scroll--;
                }
                return;
            }
            if (c == 's' || c == 'S') {
                if (current_view == VIEW_CHANNEL)
                    ch_scroll++;
                else
                    chat_scroll++;
                return;
            }
            // Tab falls through (ESC is handled at the top of handle_key).
        }
    }

    if (c == '\t') {
        if (!edit_mode) {
            // Tab cycles through the four classic views only; home isn't part
            // of the tab carousel (it's the landing screen with its own header).
            // From home, Tab jumps to Settings (the first classic tab).
            if (current_view == VIEW_HOME) {
                current_view = VIEW_SETTINGS;
            } else {
                current_view = (app_view_t)((int)(current_view + 1) % VIEW_TAB_COUNT);
            }
            if (current_view == VIEW_CHAT) {
                dm_inbox_mode  = true;
                led_dm_pending = false;
                update_notification_led();
            }
            if (current_view == VIEW_CHANNEL) {
                channel_list_mode   = true;
                channel_adding      = false;
                led_channel_pending = false;
                update_notification_led();
            }
        }
        return;
    }

    // Keyboard ESC was handled at the top of handle_key (exit only from home).

    // All remaining keys (W/S/A/D/F/L/Q/Enter/R/`<>,.`/D) are view-specific.
    switch (current_view) {
        case VIEW_HOME:
            key_home(c);
            break;
        case VIEW_SETTINGS:
            key_settings(c);
            break;
        case VIEW_NODES:
            key_nodes(c);
            break;
        case VIEW_CHAT:
            key_chat(c);
            break;
        case VIEW_CHANNEL:
            key_channel(c);
            break;
        case VIEW_MAP:
            key_map(c);
            break;
        case VIEW_TOOLBOX:
            key_toolbox(c);
            break;
        case VIEW_TOOLBOX_LOG:
            key_toolbox_log(c);
            break;
        case VIEW_TOOLBOX_COVERAGE:
            key_toolbox_coverage(c);
            break;
        default:
            break;
    }
}

// Pan + zoom keymap. Arrow-key pan lives in nav_map via the navigation event
// path; here we handle the WASD aliases for pan plus '+' / '-' for zoom and
// 'L' for the lock-to-position toggle.
static void key_map(char c) {
    if (c == 'w' || c == 'W')
        map_state_pan(0, -1);
    else if (c == 's' || c == 'S')
        map_state_pan(0, 1);
    else if (c == 'a' || c == 'A')
        map_state_pan(-1, 0);
    else if (c == 'd' || c == 'D')
        map_state_pan(1, 0);
    else if (c == '+' || c == '=')
        map_state_zoom(1);  // '=' is the unshifted '+' key
    else if (c == '-' || c == '_')
        map_state_zoom(-1);
    else if (c == 'l' || c == 'L')
        map_state_toggle_lock();
}

// ── BLE pairing UI hooks ────────────────────────────────────────────────────
// Strong overrides of the weak symbols in ble_companion.c, called from the
// NimBLE host task when SMP wants us to display the 6-digit passkey for the
// iPhone user to type. We surface it via the existing toast system with a
// 60 s duration so the user has time to read + type. Toast is currently
// rendered on home view only; for now the recommendation is to be on home
// when pairing -- a dedicated VIEW_BLE_PAIR overlay is task #23 follow-up.
//
// Touching toast_text / toast_start_ms from the NimBLE task races with the
// UI task in theory; in practice writes are 64 B / 4 B atomic enough that
// the worst case is a half-rendered character for one frame. The mutex
// dance to make this strictly correct is not worth it for a 6-digit value.
void ble_companion_show_passkey(uint32_t passkey) {
    snprintf(toast_text, sizeof(toast_text), "BLE pair: %06lu", (unsigned long)passkey);
    toast_start_ms    = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    toast_duration_ms = 60000;  // 60 s for the user to read + type on iPhone
    ESP_LOGI("ble-ui", "passkey %06lu shown on toast", (unsigned long)passkey);
}

void ble_companion_pair_done(bool success) {
    // Replace the long passkey toast with a 2 s confirmation. Reset duration
    // here so the next regular toast keeps the usual 2 s dismissal.
    snprintf(toast_text, sizeof(toast_text), "BLE pair: %s", success ? "OK" : "FAILED");
    toast_start_ms    = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    toast_duration_ms = 2000;
}
