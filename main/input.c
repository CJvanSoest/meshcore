// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "input.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

#include "bsp/device.h"
#include "bsp/input.h"
#include "bsp/led.h"
#include "esp_log.h"

#include "app_config.h"
#include "chat.h"
#include "contacts.h"
#include "history.h"
#include "identity.h"
#include "nodes.h"
#include "radio.h"
#include "settings_nvs.h"
#include "ui_state.h"

static const char *TAG = "input";

// enter_radio_bootloader lives in render.c (it draws a frame before flipping).
extern void enter_radio_bootloader(void);

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
        default:
            break;
    }
    dirty = true;
}

// ── Settings: text-field edit helpers (shared between handle_nav & handle_key)
static void settings_begin_text_edit(field_t f) {
    const char *src = "";
    if (f == FIELD_OWNER && owner_name[0] && owner_name[0] != '(') src = owner_name;
    else if (f == FIELD_ADV_NAME && lora_advert_name[0])           src = lora_advert_name;
    else if (f == FIELD_REGION_SCOPE && region_scope[0])           src = region_scope;
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
    }
    field_editing_text = false;
}

void handle_nav(uint32_t key) {
    if (radio_bootloader_mode) {
        if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
        return;
    }

    if (qr_overlay_active) {
        qr_overlay_active = false;
        return;
    }

    if (key == BSP_INPUT_NAVIGATION_KEY_F1 || key == BSP_INPUT_NAVIGATION_KEY_ESC) {
        if (edit_mode) {
            edit_mode          = false;
            field_editing_text = false;
            dirty              = false;
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
            load_lora_config();
        } else if (chat_typing) {
            // ESC during typing is cancelled by handle_key; don't fall through.
        } else if (current_view == VIEW_CHAT && !dm_inbox_mode) {
            dm_inbox_mode = true;
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_UP) {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, +1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor > 0) node_cursor--;
        } else if (current_view == VIEW_CHAT && dm_inbox_mode) {
            if (dm_inbox_cursor > 0) dm_inbox_cursor--;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_DOWN) {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected + 1) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, -1);
        } else if (current_view == VIEW_NODES) {
            int upper = node_count + contact_count - 1;
            if (upper < 0) upper = 0;
            if (node_cursor < upper) node_cursor++;
        } else if (current_view == VIEW_CHAT && dm_inbox_mode) {
            int upper = (dm_target_set ? 1 : 0) + contact_count - 1;
            if (upper < 0) upper = 0;
            if (dm_inbox_cursor < upper) dm_inbox_cursor++;
        }
    } else if (key == BSP_INPUT_NAVIGATION_KEY_LEFT) {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, -1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RIGHT) {
        if (current_view == VIEW_SETTINGS && edit_mode && !field_editing_text) field_adjust(selected, +1);
    } else if (key == BSP_INPUT_NAVIGATION_KEY_RETURN) {
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
                    ch_add_message(chat_input, true);
                } else if (dm_target_set) {
                    send_dm_message(chat_input, dm_target_pub);
                    chat_add_dm(chat_input, true, dm_target_pub);
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
                current_view   = VIEW_CHAT;
                dm_inbox_mode  = false;
                led_dm_pending = false;
                update_notification_led();
            }
        } else if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) {
                edit_mode = true;
                if (selected == FIELD_OWNER || selected == FIELD_ADV_NAME ||
                    selected == FIELD_REGION_SCOPE) {
                    settings_begin_text_edit(selected);
                }
            } else {
                if (field_editing_text)        settings_commit_text_edit(selected);
                else                            save_lora_config();
                edit_mode = false;
                dirty     = false;
            }
        }
    }
}

void handle_key(char c) {
    if (radio_bootloader_mode) {
        if (c == 27) {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
        return;
    }

    if (qr_overlay_active) {
        if (c == 27) qr_overlay_active = false;
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

    // Chat / Channel view input — intercept everything when typing.
    if (current_view == VIEW_CHAT || current_view == VIEW_CHANNEL) {
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
                        send_dm_message(chat_input, dm_target_pub);
                        chat_add_dm(chat_input, true, dm_target_pub);
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
            current_view = (app_view_t)((int)(current_view + 1) % VIEW_COUNT);
            if (current_view == VIEW_CHAT) {
                dm_inbox_mode  = true;
                led_dm_pending = false;
                update_notification_led();
            }
            if (current_view == VIEW_CHANNEL) {
                led_channel_pending = false;
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
            load_lora_config();
        } else if (current_view == VIEW_CHAT && !dm_inbox_mode) {
            dm_inbox_mode = true;
        } else {
            bsp_led_set_mode(true);
            bsp_device_restart_to_launcher();
        }
    } else if (c == 'w' || c == 'W') {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected - 1 + FIELD_COUNT) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, +1);
        } else if (current_view == VIEW_NODES) {
            if (node_cursor > 0) node_cursor--;
        }
    } else if (c == 's' || c == 'S') {
        if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) selected = (selected + 1) % FIELD_COUNT;
            else if (!field_editing_text) field_adjust(selected, -1);
        } else if (current_view == VIEW_NODES) {
            int upper = node_count + contact_count - 1;
            if (upper < 0) upper = 0;
            if (node_cursor < upper) node_cursor++;
        }
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
                current_view   = VIEW_CHAT;
                dm_inbox_mode  = false;
                led_dm_pending = false;
                update_notification_led();
            }
        } else if (current_view == VIEW_SETTINGS) {
            if (!edit_mode) {
                edit_mode = true;
                if (selected == FIELD_OWNER || selected == FIELD_ADV_NAME ||
                    selected == FIELD_REGION_SCOPE) {
                    settings_begin_text_edit(selected);
                }
            } else {
                if (field_editing_text)        settings_commit_text_edit(selected);
                else                            save_lora_config();
                edit_mode = false;
                dirty     = false;
            }
        }
    } else if (c == 'r' || c == 'R') {
        if (current_view == VIEW_SETTINGS) {
            load_owner_name();
            load_lora_advert_name();
            load_region_scope();
            load_lora_config();
            dirty              = false;
            edit_mode          = false;
            field_editing_text = false;
        } else if (current_view == VIEW_CHANNEL && !chat_typing) {
            // Wipe channel history (RAM ring + on-disk file).
            if (xSemaphoreTake(ch_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                memset(ch_msgs, 0, sizeof(ch_msgs));
                ch_head    = 0;
                ch_count   = 0;
                ch_scroll  = 0;
                xSemaphoreGive(ch_mutex);
            }
            history_delete_channel();
            ESP_LOGI(TAG, "Channel history cleared by user (R)");
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
    } else if ((c == 'u' || c == 'U') && !edit_mode && current_view == VIEW_SETTINGS) {
        enter_radio_bootloader();
    }
}
