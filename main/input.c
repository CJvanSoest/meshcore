// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "bsp/device.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "esp_log.h"

#include "app_config.h"
#include "chat.h"
#include "channels.h"
#include "contacts.h"
#include "emoji.h"
#include "history.h"
#include "identity.h"
#include "nodes.h"
#include "radio.h"
#include "region_limits.h"
#include "render_internal.h"
#include "settings_nvs.h"
#include "ui_state.h"

static const char *TAG = "input";

// Open the tile under the home-cursor: switch current_view to its target
// and initialise per-view modal state (DM inbox, channel list, etc.). TBD
// placeholder tiles report VIEW_HOME and are a no-op here.
static void open_home_tile(int idx) {
    home_action_t action = home_tile_action(idx);
    app_view_t    target = home_tile_target(idx);

    // Action tiles fire inline and keep the user on the home screen so the
    // tile-grid stays the "I am here" anchor while the toast confirms.
    if (action == HOME_ACTION_SEND_ADVERT) {
        send_advert();
        snprintf(toast_text, sizeof(toast_text), "Flood advert sent");
        toast_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        return;
    }

    if (target == VIEW_HOME) return;
    current_view = target;
    if (target == VIEW_CHAT) {
        dm_inbox_mode  = true;
        led_dm_pending = false;
        update_notification_led();
    } else if (target == VIEW_CHANNEL) {
        channel_list_mode    = true;
        channel_adding       = false;
        led_channel_pending  = false;
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
            if (sf < 5)  sf = 5;
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
            if (p < 2)  p = 2;
            if (p > 22) p = 22;
            lora_cfg.power = (uint8_t)p;
            break;
        }
        case FIELD_SYNC:
            lora_cfg.sync_word = (uint8_t)((lora_cfg.sync_word + delta) & 0xFF);
            break;
        case FIELD_PREAMBLE: {
            int pre = (int)lora_cfg.preamble_length + delta;
            if (pre < 2)     pre = 2;
            if (pre > 65535) pre = 65535;
            lora_cfg.preamble_length = (uint16_t)pre;
            break;
        }
        case FIELD_ADVERT_INT: {
            static const uint16_t presets[] = {30, 60, 300, 900};
            const int n = (int)(sizeof(presets) / sizeof(presets[0]));
            int idx = 2;  // default to 5 min
            for (int i = 0; i < n; i++) if (presets[i] == advert_interval_s) { idx = i; break; }
            idx = ((idx + delta) % n + n) % n;
            advert_interval_s = presets[idx];
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
            const int n = (int)(sizeof(ROLES) / sizeof(ROLES[0]));
            int idx = 0;
            for (int i = 0; i < n; i++) if (ROLES[i] == lora_role) { idx = i; break; }
            idx = ((idx + delta) % n + n) % n;
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
        default:
            break;
    }
    dirty = true;
}

// ── Settings: text-field edit helpers (shared between handle_nav & handle_key)
static void settings_begin_text_edit(field_t f) {
    const char *src = "";
    char numbuf[24] = {0};
    if (f == FIELD_OWNER && owner_name[0] && owner_name[0] != '(') src = owner_name;
    else if (f == FIELD_ADV_NAME && lora_advert_name[0])           src = lora_advert_name;
    else if (f == FIELD_REGION_SCOPE && region_scope[0])           src = region_scope;
    else if (f == FIELD_GPS_LAT) {
        if (gps_position_valid) snprintf(numbuf, sizeof(numbuf), "%.6f", (double)gps_lat_e6 / 1e6);
        src = numbuf;
    } else if (f == FIELD_GPS_LON) {
        if (gps_position_valid) snprintf(numbuf, sizeof(numbuf), "%.6f", (double)gps_lon_e6 / 1e6);
        src = numbuf;
    }
    strncpy(field_edit_buf, src, sizeof(field_edit_buf) - 1);
    field_edit_buf[sizeof(field_edit_buf) - 1] = '\0';
    field_edit_len     = (int)strlen(field_edit_buf);
    field_editing_text = true;
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
        char *trim = field_edit_buf;
        while (*trim == ' ') trim++;
        if (*trim == '\0') {
            gps_position_valid = false;
            gps_lat_e6         = 0;
            gps_lon_e6         = 0;
        } else {
            // Accept comma as decimal separator too (Dutch keyboard habit).
            for (char *p = field_edit_buf; *p; p++) if (*p == ',') *p = '.';
            double  v    = atof(field_edit_buf);
            int32_t v_e6 = (int32_t)(v * 1e6);
            if (f == FIELD_GPS_LAT) gps_lat_e6 = v_e6;
            else                    gps_lon_e6 = v_e6;
            // Mark valid once at least one axis has been set non-zero, OR if
            // both keys already had values (allowing fine-tune of one axis).
            if (gps_lat_e6 != 0 || gps_lon_e6 != 0) gps_position_valid = true;
        }
        save_gps_coords();
    }
    field_editing_text = false;
}

void handle_nav(uint32_t key) {
    if (qr_overlay_active) {
        qr_overlay_active = false;
        if (qr_from_home) {
            qr_from_home = false;
            current_view = VIEW_HOME;
        }
        return;
    }

    // Emoji picker (F4 button = green circle on Tanmatsu) opens during chat typing.
    if (key == BSP_INPUT_NAVIGATION_KEY_F4 && chat_typing &&
        (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL) &&
        !emoji_picker_active) {
        emoji_picker_active = true;
        emoji_picker_cursor = 0;
        return;
    }

    // Emoji picker navigation via D-pad / RETURN / ESC. Without this branch
    // selecting on Tanmatsu's D-pad (which fires RETURN as nav-event, not as
    // '\r' in handle_key) wouldn't close the picker — leaving it hanging open
    // for the next chat-typing session.
    if (emoji_picker_active && chat_typing &&
        (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL)) {
        const int cols = 4;
        // ESC/F1 close; F4 (the green circle) toggles — pressing it again closes
        // the picker instead of leaving it stuck open (which blocked leaving chat).
        if (key == BSP_INPUT_NAVIGATION_KEY_ESC || key == BSP_INPUT_NAVIGATION_KEY_F1 ||
            key == BSP_INPUT_NAVIGATION_KEY_F4) {
            emoji_picker_active = false;
            return;
        }
        if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
            int idx = emoji_picker_cursor;
            if (idx >= 0 && idx < EMOJI_COUNT) {
                const emoji_entry_t *e = &EMOJI_SET[idx];
                if (chat_input_len + e->utf8_len <= MAX_INPUT_LEN) {
                    memcpy(&chat_input[chat_input_len], e->utf8, e->utf8_len);
                    chat_input_len           += e->utf8_len;
                    chat_input[chat_input_len] = '\0';
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

    if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
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
            // ESC during typing is cancelled by handle_key; don't fall through.
        } else if (current_view == VIEW_CHAT && !dm_inbox_mode) {
            dm_inbox_mode = true;
        } else if (current_view == VIEW_CHANNEL && channel_adding) {
            channel_adding     = false;
            field_editing_text = false;
            field_edit_len     = 0;
            field_edit_buf[0]  = '\0';
        } else if (current_view == VIEW_CHANNEL && !channel_list_mode) {
            channel_list_mode = true;
        } else if (current_view == VIEW_SETTINGS && !settings_category_list_mode) {
            // First ESC out of a drilled-in settings category returns to the
            // category list; second ESC then falls through to HOME.
            settings_category_list_mode = true;
            settings_scroll             = 0;
        } else if (current_view != VIEW_HOME) {
            // ESC from any non-modal view returns to the home tile-grid before
            // bouncing back to the launcher (so home becomes the safe "back").
            current_view = VIEW_HOME;
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (current_view == VIEW_HOME) {
            int cols = 4;  // mirrors HOME_TILE_COLS in render_home.c
            if (home_cursor - cols >= 0) home_cursor -= cols;
        } else if (current_view == VIEW_SETTINGS) {
            if (settings_category_list_mode) {
                if (settings_category_cursor > 0) settings_category_cursor--;
            } else if (!edit_mode) {
                int first, last;
                settings_category_bounds(settings_category_active, &first, &last);
                int n = last - first + 1;
                selected = first + (selected - first - 1 + n) % n;
            } else if (!field_editing_text) field_adjust(selected, +1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor > 0) node_cursor--;
        } else if (current_view == VIEW_CHAT && dm_inbox_mode) {
            if (dm_inbox_cursor > 0) dm_inbox_cursor--;
        } else if (current_view == VIEW_CHANNEL && channel_list_mode && !channel_adding) {
            if (channel_list_cursor > 0) channel_list_cursor--;
        } else if (current_view == VIEW_CHAT && !dm_inbox_mode) {
            if (chat_scroll > 0) chat_scroll--;          // scroll up through DM history
        } else if (current_view == VIEW_CHANNEL && !channel_list_mode && !channel_adding) {
            if (ch_scroll > 0) ch_scroll--;              // scroll up through channel history
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        if (current_view == VIEW_HOME) {
            int cols = 4;
            int total = home_tile_count();
            if (home_cursor + cols < total) home_cursor += cols;
        } else if (current_view == VIEW_SETTINGS) {
            if (settings_category_list_mode) {
                if (settings_category_cursor < settings_category_count() - 1)
                    settings_category_cursor++;
            } else if (!edit_mode) {
                int first, last;
                settings_category_bounds(settings_category_active, &first, &last);
                int n = last - first + 1;
                selected = first + (selected - first + 1) % n;
            } else if (!field_editing_text) field_adjust(selected, -1);
        } else if (current_view == VIEW_NODES) {
            int upper = node_count + contact_count - 1;
            if (upper < 0) upper = 0;
            if (node_cursor < upper) node_cursor++;
        } else if (current_view == VIEW_CHAT && dm_inbox_mode) {
            int upper = (dm_target_set ? 1 : 0) + contact_count - 1;
            if (upper < 0) upper = 0;
            if (dm_inbox_cursor < upper) dm_inbox_cursor++;
        } else if (current_view == VIEW_CHANNEL && channel_list_mode && !channel_adding) {
            if (channel_list_cursor < channel_count - 1) channel_list_cursor++;
        } else if (current_view == VIEW_CHAT && !dm_inbox_mode) {
            chat_scroll++;                                // scroll down (render clamps to newest)
        } else if (current_view == VIEW_CHANNEL && !channel_list_mode && !channel_adding) {
            ch_scroll++;                                  // scroll down (render clamps to newest)
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
        if (current_view == VIEW_HOME) {
            if (home_cursor > 0) home_cursor--;
        } else if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, -1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
        if (current_view == VIEW_HOME) {
            if (home_cursor < home_tile_count() - 1) home_cursor++;
        } else if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, +1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
        if (current_view == VIEW_HOME) {
            open_home_tile(home_cursor);
            return;
        }
        if (current_view == VIEW_CHANNEL && channel_adding) {
            if (field_edit_len > 0) {
                char name[CHANNEL_NAME_MAX_LEN + 1];
                name[0] = '\0';
                bool needs_hash = (field_edit_buf[0] != '#');
                int  cap        = CHANNEL_NAME_MAX_LEN - (needs_hash ? 1 : 0);
                if (needs_hash) { name[0] = '#'; name[1] = '\0'; }
                strncat(name, field_edit_buf, cap);
                int slot = channels_add_by_name(name);
                if (slot > 0) channel_list_cursor = slot;
            }
            channel_adding     = false;
            field_editing_text = false;
            field_edit_len     = 0;
            field_edit_buf[0]  = '\0';
            return;
        }
        if (current_view == VIEW_CHANNEL && channel_list_mode && !channel_adding) {
            if (channel_list_cursor >= 0 && channel_list_cursor < channel_count &&
                channels[channel_list_cursor].active) {
                ch_select_channel(channel_list_cursor);  // clears + reloads this channel's history
                channel_list_mode  = false;
            }
            return;
        }
        if (current_view == VIEW_CHAT && dm_inbox_mode && !chat_typing) {
            int idx_map[MAX_CONTACTS + 1];
            int idx_count = 0;
            bool active_on_top = dm_target_set;
            if (active_on_top) idx_map[idx_count++] = -1;
            for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
                if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0)
                    continue;
                idx_map[idx_count++] = i;
            }
            if (dm_inbox_cursor < idx_count) {
                int e = idx_map[dm_inbox_cursor];
                if (e >= 0) {
                    dm_select_target(contacts[e].pub_key, contacts[e].alias);
                }
                dm_inbox_mode = false;
                led_dm_pending = false;
                update_notification_led();
            }
            return;
        }
        if ((current_view == VIEW_CHAT || current_view == VIEW_CHANNEL) && chat_typing) {
            if (chat_input_len > 0) {
                if (current_view == VIEW_CHANNEL) {
                    send_chat_message(chat_input);
                    // Channel context lives in the chat header (active channel
                    // name + region) — no need for an inline [#name] prefix.
                    ch_add_message(chat_input, true);
                } else if (dm_target_set) {
                    uint8_t ack_crc[4] = {0};
                    send_dm_message(chat_input, dm_target_pub, ack_crc);
                    chat_add_dm(chat_input, true, dm_target_pub);
                    chat_arm_ack_dm(ack_crc);
                    meshcore_device_role_t r = MESHCORE_DEVICE_ROLE_CHAT_NODE;
                    if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        for (int ni = 0; ni < MAX_NODES; ni++) {
                            if (node_list[ni].active &&
                                memcmp(node_list[ni].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0) {
                                r = node_list[ni].role; break;
                            }
                        }
                        xSemaphoreGive(node_mutex);
                    }
                    contact_ensure(dm_target_pub, dm_target_name, (uint8_t)r);
                } else {
                    chat_add_message("(geen DM-target — kies een node in Nodes-tab)", false);
                }
                chat_input_len = 0;
                chat_input[0]  = '\0';
            }
            chat_typing = false;
        } else if (current_view == VIEW_NODES) {
            if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
                int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
                if (node_cursor < idx_count) {
                    display_row_t *d = &rows_dl[node_cursor];
                    if (d->node_idx >= 0) {
                        node_entry_t *n = &node_list[d->node_idx];
                        dm_select_target(n->pub_key, n->name);
                        contact_ensure(n->pub_key, n->name, (uint8_t)n->role);
                    } else if (d->is_contact) {
                        contact_t *c = &contacts[d->contact_idx];
                        dm_select_target(c->pub_key, c->alias);
                    }
                }
                xSemaphoreGive(node_mutex);
            }
            if (dm_target_set) {
                current_view    = VIEW_CHAT;
                dm_inbox_mode   = false;
                led_dm_pending  = false;
                update_notification_led();
            }
        } else if (current_view == VIEW_SETTINGS) {
            if (settings_category_list_mode) {
                // Drill into the focused category: clamp the field cursor to
                // the first field of that category so the row-renderer starts
                // at a valid position.
                settings_category_active    = settings_category_cursor;
                settings_category_list_mode = false;
                int first, last;
                settings_category_bounds(settings_category_active, &first, &last);
                selected = first;
                settings_scroll = 0;
                return;
            }
            // FIELD_RADIO_FW / FIELD_RADIO_FW_APP / FIELD_DUTY_CYCLE are read-only — Enter no-op.
            // FIELD_ANTENNA_GAIN is read-only until country is set (otherwise gain has no effect).
            bool gain_locked = (selected == FIELD_ANTENNA_GAIN &&
                                (country_code[0] == '-' || country_code[0] == '\0'));
            if (selected == FIELD_RADIO_FW || selected == FIELD_RADIO_FW_APP ||
                selected == FIELD_DUTY_CYCLE || gain_locked) {
                // ignore
            } else if (!edit_mode) {
                edit_mode = true;
                if (selected == FIELD_OWNER || selected == FIELD_ADV_NAME ||
                    selected == FIELD_REGION_SCOPE ||
                    selected == FIELD_GPS_LAT || selected == FIELD_GPS_LON) {
                    settings_begin_text_edit(selected);
                }
            } else {
                if (field_editing_text)              settings_commit_text_edit(selected);
                else if (selected == FIELD_COUNTRY)  save_country_code();
                else if (selected == FIELD_ANTENNA_GAIN) save_antenna_gain();
                else                                  save_lora_config();
                edit_mode = false;
                dirty     = false;
            }
        }
    }
}

void handle_key(char c) {
    if (qr_overlay_active) {
        if (c == 27) {
            qr_overlay_active = false;
            if (qr_from_home) {
                qr_from_home = false;
                current_view = VIEW_HOME;
            }
        }
        return;
    }

    // Settings text-edit intercepts all printables so the global W/S/T/F/L/Q/R
    // shortcuts don't fire while typing.
    if (current_view == VIEW_SETTINGS && edit_mode && field_editing_text) {
        if (c == 27) {  // ESC: cancel
            edit_mode          = false;
            field_editing_text = false;
            dirty              = false;
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
        } else if (c == '\r' || c == '\n') {  // Enter: save
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
            field_edit_buf[field_edit_len++] = c;
            field_edit_buf[field_edit_len]   = '\0';
        }
        return;
    }

    // DM inbox view: own keymap (no typing here).
    if (current_view == VIEW_CHAT && dm_inbox_mode && !chat_typing) {
        if (c == 'w' || c == 'W') { if (dm_inbox_cursor > 0) dm_inbox_cursor--; return; }
        if (c == 's' || c == 'S') {
            int upper = (dm_target_set ? 1 : 0) + contact_count - 1;
            if (upper < 0) upper = 0;
            if (dm_inbox_cursor < upper) dm_inbox_cursor++;
            return;
        }
        // T to type only works inside the chat view; here we ignore it.
        if (c == 't' || c == 'T') return;
        // Tab and ESC fall through.
    }

    // Channel list-mode: own keymap (W/S nav, Enter open, A add, D delete) plus
    // text-input intercept when channel_adding is active. Must come before the
    // chat-typing branch so chat_typing keys don't fire here.
    if (current_view == VIEW_CHANNEL && channel_list_mode) {
        if (channel_adding) {
            if (c == 27) {
                channel_adding     = false;
                field_editing_text = false;
                field_edit_len     = 0;
                field_edit_buf[0]  = '\0';
                return;
            }
            if (c == '\r' || c == '\n') {
                if (field_edit_len > 0) {
                    // Auto-prefix with '#' if user typed plain text — non-Public
                    // channels always start with '#' in MeshCore name convention.
                    // Buf is sizeof(field_edit_buf)=33; cap input slice at
                    // CHANNEL_NAME_MAX_LEN-1 so even after prefix we fit.
                    char name[CHANNEL_NAME_MAX_LEN + 1];
                    name[0] = '\0';
                    bool needs_hash = (field_edit_buf[0] != '#');
                    int  cap        = CHANNEL_NAME_MAX_LEN - (needs_hash ? 1 : 0);
                    if (needs_hash) { name[0] = '#'; name[1] = '\0'; }
                    strncat(name, field_edit_buf, cap);
                    int slot = channels_add_by_name(name);
                    if (slot > 0) {
                        channel_list_cursor = slot;
                    }
                }
                channel_adding     = false;
                field_editing_text = false;
                field_edit_len     = 0;
                field_edit_buf[0]  = '\0';
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
            field_editing_text = true;
            field_edit_len     = 0;
            field_edit_buf[0]  = '\0';
            return;
        }
        if (c == 'd' || c == 'D') {
            // Public (idx 0) cannot be removed.
            if (channel_list_cursor > 0 && channel_list_cursor < channel_count) {
                int removed = channel_list_cursor;
                channels_remove(removed);
                if (channel_list_cursor >= channel_count) channel_list_cursor = channel_count - 1;
                if (channel_list_cursor < 0)              channel_list_cursor = 0;
            }
            return;
        }
        if (c == '\r' || c == '\n') {
            if (channel_list_cursor >= 0 && channel_list_cursor < channel_count &&
                channels[channel_list_cursor].active) {
                ch_select_channel(channel_list_cursor);  // clears + reloads this channel's history
                channel_list_mode  = false;
            }
            return;
        }
        // Tab and ESC fall through.
    }

    // Chat / Channel view input — intercept everything when typing.
    if (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL) {
        // Emoji picker overlay swallows all keys when active.
        if (chat_typing && emoji_picker_active) {
            const int cols = 4;
            if (c == 27) {
                emoji_picker_active = false;
                return;
            }
            if (c == '\r' || c == '\n') {
                int idx = emoji_picker_cursor;
                if (idx >= 0 && idx < EMOJI_COUNT) {
                    const emoji_entry_t *e = &EMOJI_SET[idx];
                    if (chat_input_len + e->utf8_len <= MAX_INPUT_LEN) {
                        memcpy(&chat_input[chat_input_len], e->utf8, e->utf8_len);
                        chat_input_len           += e->utf8_len;
                        chat_input[chat_input_len] = '\0';
                    }
                }
                emoji_picker_active = false;
                return;
            }
            if (c == 'a' || c == 'A') { if (emoji_picker_cursor > 0)                  emoji_picker_cursor--;       return; }
            if (c == 'd' || c == 'D') { if (emoji_picker_cursor < EMOJI_COUNT - 1)    emoji_picker_cursor++;       return; }
            if (c == 'w' || c == 'W') { if (emoji_picker_cursor - cols >= 0)          emoji_picker_cursor -= cols; return; }
            if (c == 's' || c == 'S') { if (emoji_picker_cursor + cols < EMOJI_COUNT) emoji_picker_cursor += cols; return; }
            return;  // swallow the rest
        }
        if (chat_typing) {
            if (c == 27) {
                chat_typing    = false;
                chat_input_len = 0;
                chat_input[0]  = '\0';
            } else if (c == '\r' || c == '\n') {
                if (chat_input_len > 0) {
                    if (current_view == VIEW_CHANNEL) {
                        send_chat_message(chat_input);
                        ch_add_message(chat_input, true);
                    } else if (dm_target_set) {
                        uint8_t ack_crc[4] = {0};
                        send_dm_message(chat_input, dm_target_pub, ack_crc);
                        chat_add_dm(chat_input, true, dm_target_pub);
                        chat_arm_ack_dm(ack_crc);
                    } else {
                        send_chat_message(chat_input);
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
                if (current_view == VIEW_CHANNEL) { if (ch_scroll > 0) ch_scroll--; }
                else                              { if (chat_scroll > 0) chat_scroll--; }
                return;
            }
            if (c == 's' || c == 'S') {
                if (current_view == VIEW_CHANNEL) ch_scroll++;
                else                              chat_scroll++;
                return;
            }
            // Tab and ESC fall through.
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
                dm_inbox_mode    = true;
                led_dm_pending   = false;
                update_notification_led();
            }
            if (current_view == VIEW_CHANNEL) {
                channel_list_mode    = true;
                channel_adding       = false;
                led_channel_pending  = false;
                update_notification_led();
            }
        }
        return;
    }

    if (c == 27) {
        if (edit_mode) {
            edit_mode          = false;
            field_editing_text = false;
            dirty              = false;
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
            load_gps_coords();
            load_lora_config();
        } else if (current_view == VIEW_CHAT && !dm_inbox_mode) {
            dm_inbox_mode = true;
        } else if (current_view == VIEW_CHANNEL && !channel_list_mode) {
            channel_list_mode = true;
        } else if (current_view == VIEW_SETTINGS && !settings_category_list_mode) {
            settings_category_list_mode = true;
            settings_scroll             = 0;
        } else if (current_view != VIEW_HOME) {
            current_view = VIEW_HOME;
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (c == 'w' || c == 'W') {
        if (current_view == VIEW_HOME) {
            int cols = 4;
            if (home_cursor - cols >= 0) home_cursor -= cols;
        } else if (current_view == VIEW_SETTINGS) {
            if (settings_category_list_mode) {
                if (settings_category_cursor > 0) settings_category_cursor--;
            } else if (!edit_mode) {
                int first, last;
                settings_category_bounds(settings_category_active, &first, &last);
                int n = last - first + 1;
                selected = first + (selected - first - 1 + n) % n;
            } else if (!field_editing_text) field_adjust(selected, +1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor > 0) node_cursor--;
        }
    } else if (c == 's' || c == 'S') {
        if (current_view == VIEW_HOME) {
            int cols = 4;
            int total = home_tile_count();
            if (home_cursor + cols < total) home_cursor += cols;
        } else if (current_view == VIEW_SETTINGS) {
            if (settings_category_list_mode) {
                if (settings_category_cursor < settings_category_count() - 1)
                    settings_category_cursor++;
            } else if (!edit_mode) {
                int first, last;
                settings_category_bounds(settings_category_active, &first, &last);
                int n = last - first + 1;
                selected = first + (selected - first + 1) % n;
            } else if (!field_editing_text) field_adjust(selected, -1);
        } else if (current_view == VIEW_NODES) {
            int upper = node_count + contact_count - 1;
            if (upper < 0) upper = 0;
            if (node_cursor < upper) node_cursor++;
        }
    } else if ((c == 'a' || c == 'A') && current_view == VIEW_HOME) {
        if (home_cursor > 0) home_cursor--;
    } else if ((c == 'd' || c == 'D') && current_view == VIEW_HOME) {
        if (home_cursor < home_tile_count() - 1) home_cursor++;
    } else if ((c == 'a' || c == 'A') && current_view == VIEW_NODES) {
        send_advert();
    } else if ((c == 'f' || c == 'F') && current_view == VIEW_NODES) {
        if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
            int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
            if (node_cursor < idx_count) {
                display_row_t *d = &rows_dl[node_cursor];
                if (d->is_contact) {
                    contact_toggle(contacts[d->contact_idx].pub_key, NULL, 0);
                } else if (d->node_idx >= 0) {
                    node_entry_t *n = &node_list[d->node_idx];
                    int r = contact_toggle(n->pub_key, n->name, (uint8_t)n->role);
                    if (r < 0) ESP_LOGW(TAG, "Contacts list is full (%d/%d)", contact_count, MAX_CONTACTS);
                }
            }
            xSemaphoreGive(node_mutex);
        }
    } else if ((c == 'l' || c == 'L') && current_view == VIEW_NODES) {
        static const meshcore_device_role_t cycle[] = {
            MESHCORE_DEVICE_ROLE_UNKNOWN,
            MESHCORE_DEVICE_ROLE_CHAT_NODE,
            MESHCORE_DEVICE_ROLE_REPEATER,
            MESHCORE_DEVICE_ROLE_ROOM_SERVER,
            MESHCORE_DEVICE_ROLE_SENSOR,
        };
        const int n = (int)(sizeof(cycle) / sizeof(cycle[0]));
        int idx = 0;
        for (int i = 0; i < n; i++) if (cycle[i] == node_filter) { idx = i; break; }
        node_filter = cycle[(idx + 1) % n];
        node_scroll = 0;
        node_cursor = 0;
    } else if ((c == 'q' || c == 'Q') && current_view == VIEW_NODES && identity_is_ready()) {
        qr_overlay_active = true;
    } else if (c == '<' || c == ',') {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, -1);
    } else if (c == '>' || c == '.') {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, +1);
    } else if (c == '\r' || c == '\n') {
        if (current_view == VIEW_HOME) {
            open_home_tile(home_cursor);
            return;
        }
        if (current_view == VIEW_NODES) {
            if (xSemaphoreTake(node_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                display_row_t rows_dl[MAX_CONTACTS + MAX_NODES];
                int idx_count = build_node_display(rows_dl, MAX_CONTACTS + MAX_NODES);
                if (node_cursor < idx_count) {
                    display_row_t *d = &rows_dl[node_cursor];
                    if (d->node_idx >= 0) {
                        node_entry_t *n = &node_list[d->node_idx];
                        dm_select_target(n->pub_key, n->name);
                        contact_ensure(n->pub_key, n->name, (uint8_t)n->role);
                    } else if (d->is_contact) {
                        contact_t *c2 = &contacts[d->contact_idx];
                        dm_select_target(c2->pub_key, c2->alias);
                    }
                }
                xSemaphoreGive(node_mutex);
            }
            if (dm_target_set) {
                current_view    = VIEW_CHAT;
                dm_inbox_mode   = false;
                led_dm_pending  = false;
                update_notification_led();
            }
        } else if (current_view == VIEW_SETTINGS) {
            if (settings_category_list_mode) {
                settings_category_active    = settings_category_cursor;
                settings_category_list_mode = false;
                int first, last;
                settings_category_bounds(settings_category_active, &first, &last);
                selected = first;
                settings_scroll = 0;
                return;
            }
            // FIELD_RADIO_FW / FIELD_RADIO_FW_APP / FIELD_DUTY_CYCLE are read-only — Enter no-op.
            // FIELD_ANTENNA_GAIN is read-only until country is set (otherwise gain has no effect).
            bool gain_locked = (selected == FIELD_ANTENNA_GAIN &&
                                (country_code[0] == '-' || country_code[0] == '\0'));
            if (selected == FIELD_RADIO_FW || selected == FIELD_RADIO_FW_APP ||
                selected == FIELD_DUTY_CYCLE || gain_locked) {
                // ignore
            } else if (!edit_mode) {
                edit_mode = true;
                if (selected == FIELD_OWNER || selected == FIELD_ADV_NAME ||
                    selected == FIELD_REGION_SCOPE ||
                    selected == FIELD_GPS_LAT || selected == FIELD_GPS_LON) {
                    settings_begin_text_edit(selected);
                }
            } else {
                if (field_editing_text)              settings_commit_text_edit(selected);
                else if (selected == FIELD_COUNTRY)  save_country_code();
                else if (selected == FIELD_ANTENNA_GAIN) save_antenna_gain();
                else                                  save_lora_config();
                edit_mode = false;
                dirty     = false;
            }
        }
    } else if (c == 'r' || c == 'R') {
        if (current_view == VIEW_SETTINGS) {
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
            load_gps_coords();
            load_lora_config();
            dirty              = false;
            edit_mode          = false;
            field_editing_text = false;
        } else if (current_view == VIEW_CHANNEL && !chat_typing && !channel_list_mode) {
            // Wipe ONLY the active channel's history (RAM ring + its own file).
            if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                memset(ch_msgs, 0, sizeof(ch_msgs));
                ch_head    = 0;
                ch_count   = 0;
                ch_scroll  = 0;
                xSemaphoreGive(ch_mutex);
            }
            if (active_channel_idx >= 0 && active_channel_idx < channel_count &&
                channels[active_channel_idx].active) {
                history_delete_channel(channels[active_channel_idx].secret);
            }
            ESP_LOGI(TAG, "Channel history cleared by user (R): %s",
                     channels[active_channel_idx].name);
        }
    } else if ((c == 'd' || c == 'D') && current_view == VIEW_CHAT && dm_inbox_mode && !chat_typing) {
        // Delete selected DM conversation: history file + contact from NVS.
        // Contact is re-added automatically on next DM from that peer.
        int idx_map[MAX_CONTACTS + 1];
        int idx_count = 0;
        bool active_on_top = dm_target_set;
        if (active_on_top) idx_map[idx_count++] = -1;
        for (int i = 0; i < contact_count && idx_count < MAX_CONTACTS + 1; i++) {
            if (active_on_top && memcmp(contacts[i].pub_key, dm_target_pub, MESHCORE_PUB_KEY_SIZE) == 0)
                continue;
            idx_map[idx_count++] = i;
        }
        if (dm_inbox_cursor < idx_count) {
            int e = idx_map[dm_inbox_cursor];
            uint8_t target_pub[MESHCORE_PUB_KEY_SIZE];
            char    target_name[MESHCORE_MAX_NAME_SIZE + 1];
            int     ci = -1;
            if (e == -1) {
                memcpy(target_pub, dm_target_pub, MESHCORE_PUB_KEY_SIZE);
                strncpy(target_name, dm_target_name, sizeof(target_name) - 1);
                target_name[sizeof(target_name) - 1] = '\0';
                ci = contact_find(dm_target_pub);
                dm_target_set = false;
                memset(chat_msgs, 0, sizeof(chat_msgs));
                chat_head = chat_count = chat_scroll = 0;
            } else {
                ci = e;
                memcpy(target_pub, contacts[ci].pub_key, MESHCORE_PUB_KEY_SIZE);
                strncpy(target_name, contacts[ci].alias, sizeof(target_name) - 1);
                target_name[sizeof(target_name) - 1] = '\0';
            }

            history_delete_dm(target_pub);
            if (ci >= 0) {
                for (int j = ci; j < contact_count - 1; j++) contacts[j] = contacts[j + 1];
                memset(&contacts[contact_count - 1], 0, sizeof(contact_t));
                contact_count--;
                contacts_save();
            }
            if (dm_inbox_cursor > 0) dm_inbox_cursor--;
            ESP_LOGI(TAG, "DM deleted by user (D): %s", target_name);
        }
    }
}
