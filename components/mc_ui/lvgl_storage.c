// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// VIEW_TOOLBOX_STORAGE.

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "appfs.h"
#include "backup.h"
#include "channels.h"
#include "contacts.h"
#include "esp_heap_caps.h"
#include "history.h"
#include "identity.h"
#include "locfs.h"
#include "lvgl.h"
#include "lvgl_internal.h"
#include "lvgl_port.h"
#include "nvs_flash.h"
#include "render.h"
#include "ui_state.h"

// ── VIEW_TOOLBOX_STORAGE ─────────────────────────────────────────────────────
// Read-only NVS/SD/app-data usage plus the manual backup/restore/factory-reset
// actions. The actions are the user-facing test path for the SD backup mirror
// (see backup.c / issue #66). Destructive actions require a second Enter.

// STORAGE_ACTION_COUNT lives in ui_state.h so input.c can size its Backup-detail
// cursor to the same set.
static const char* const storage_actions[STORAGE_ACTION_COUNT] = {
    "Backup now",
    "Restore from backup",
    "Factory reset (backup first)",
};

typedef struct {
    char ns[16];
    int  count;
} st_ns_t;

// Per-namespace NVS entry counts (logical keys, like badgelink `nvs list`),
// sorted by count desc. The partition is shared by all apps + firmware, so this
// is the honest granularity — namespaces don't map 1:1 to apps ("system" is
// shared by the launcher, firmware and us).
static int st_nvs_ns_counts(st_ns_t* out, int max) {
    nvs_iterator_t it  = NULL;
    esp_err_t      err = nvs_entry_find("nvs", NULL, NVS_TYPE_ANY, &it);
    int            n   = 0;
    while (err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        int f = -1;
        for (int i = 0; i < n; i++)
            if (strcmp(out[i].ns, info.namespace_name) == 0) {
                f = i;
                break;
            }
        if (f < 0 && n < max) {
            f = n++;
            strncpy(out[f].ns, info.namespace_name, sizeof(out[f].ns) - 1);
            out[f].ns[sizeof(out[f].ns) - 1] = '\0';
            out[f].count                     = 0;
        }
        if (f >= 0) out[f].count++;
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    for (int i = 0; i < n; i++)  // selection sort by count desc (n is tiny)
        for (int j = i + 1; j < n; j++)
            if (out[j].count > out[i].count) {
                st_ns_t t = out[i];
                out[i]    = out[j];
                out[j]    = t;
            }
    return n;
}

// "1.2 MB" / "340 KB" / "12 B" formatter. No %f (keeps the float printf path out
// of this TU) and no %llu: match the codebase idiom of dividing a 64-bit value
// down first, then printing an (unsigned long) with %lu — the nano printf path
// can't format %llu.
static void st_human_size(uint64_t b, char* out, size_t cap) {
    if (b >= 1024ULL * 1024) {
        unsigned long t = (unsigned long)((b * 10) / (1024ULL * 1024));  // tenths of a MB
        snprintf(out, cap, "%lu.%lu MB", t / 10, t % 10);
    } else if (b >= 1024) {
        snprintf(out, cap, "%lu KB", (unsigned long)(b / 1024));
    } else {
        snprintf(out, cap, "%lu B", (unsigned long)b);
    }
}

// True when a conversation log resolves to no current contact/channel — an
// orphan left behind by a removed contact or a left/deleted channel. Public
// (channel slot 0) always resolves, so it is never an orphan.
static bool st_conv_is_orphan(const history_conv_t* c) {
    return c->is_dm ? (contact_find_by_prefix(c->id) < 0) : (channels_find_by_secret_prefix(c->id) < 0);
}

// Map a conversation's on-disk id (8-byte filename prefix) to a human label: a
// contact alias for DMs, the channel name for channels. Orphans (no match) get a
// "DM?/Ch?" + hex marker so they stand out as clean-up candidates.
static void st_conv_label(const history_conv_t* c, char* out, size_t cap) {
    if (c->is_dm) {
        int idx = contact_find_by_prefix(c->id);
        if (idx >= 0 && contacts[idx].alias[0])
            snprintf(out, cap, "DM %s", contacts[idx].alias);
        else if (idx >= 0)
            snprintf(out, cap, "DM %02x%02x%02x%02x", c->id[0], c->id[1], c->id[2], c->id[3]);
        else
            snprintf(out, cap, "DM? %02x%02x%02x%02x", c->id[0], c->id[1], c->id[2], c->id[3]);
    } else {
        int idx = channels_find_by_secret_prefix(c->id);  // includes slot 0 (Public)
        if (idx >= 0)
            snprintf(out, cap, "%s", channels[idx].name);
        else
            snprintf(out, cap, "Ch? %02x%02x%02x%02x", c->id[0], c->id[1], c->id[2], c->id[3]);
    }
}

// One category summary line, formatted into `val` (the drill-down entry point).
static void st_summary_value(int cat, char* val, size_t cap) {
    switch (cat) {
        case ST_CAT_NVS: {
            nvs_stats_t st;
            if (nvs_get_stats(NULL, &st) == ESP_OK) {
                int pct = st.total_entries > 0 ? (int)((st.used_entries * 100) / st.total_entries) : 0;
                snprintf(val, cap, "%d/%d (%d%%)", (int)st.used_entries, (int)st.total_entries, pct);
            } else {
                snprintf(val, cap, "unavailable");
            }
            break;
        }
        case ST_CAT_MEM: {
            unsigned long ram_f = (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
            unsigned long ps_f  = (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
            snprintf(val, cap, "RAM %luK  PSRAM %luK free", ram_f, ps_f);
            break;
        }
        case ST_CAT_STORAGE: {
            uint64_t sd_cap = history_sd_capacity_bytes();
            char     sd[16];
            if (sd_cap > 0)
                snprintf(sd, sizeof(sd), "%luG", (unsigned long)(sd_cap / (1024ULL * 1024 * 1024)));
            else
                snprintf(sd, sizeof(sd), "none");
            snprintf(val, cap, "AppFS 8M  SD %s", sd);
            break;
        }
        case ST_CAT_HISTORY: {
            history_conv_t tmp[40];
            uint64_t       htot = 0;
            int            hn   = history_list_conversations(tmp, 40, &htot);
            char           sz[24];
            st_human_size(htot, sz, sizeof(sz));
            snprintf(val, cap, "%s (%d chats)", sz, hn);
            break;
        }
        case ST_CAT_BACKUP: {
            long bmt = backup_file_mtime();
            if (bmt > 0) {
                time_t    t = (time_t)bmt;
                struct tm tmv;
                localtime_r(&t, &tmv);
                strftime(val, cap, "%Y-%m-%d %H:%M", &tmv);
            } else {
                snprintf(val, cap, "none");
            }
            break;
        }
        default:
            val[0] = '\0';
    }
}

// Common header + footer chrome for the Storage Viewer. `sub` is the header
// subtitle (NULL on the summary); `hint` is the footer nav text; `back` is the
// ESC target label.
static void st_chrome(lv_obj_t* scr, int w, int h, const char* sub, const char* hint, const char* back) {
    add_rect(scr, 0, 0, w, ST_HEADER_H, COL_PAGER_BG);
    add_rect(scr, 0, ST_HEADER_H - 1, w, 1, COL_PAGER_ACCENT);
    char title[40];
    if (sub)
        snprintf(title, sizeof(title), "Storage / %s", sub);
    else
        snprintf(title, sizeof(title), "Storage");
    add_label(scr, 12, (ST_HEADER_H - TXT_TAB) / 2, TXT_TAB, COL_PAGER_TEXT, title);

    int fy = h - ST_FOOTER_H;
    add_rect(scr, 0, fy, w, ST_FOOTER_H, COL_HEADER);
    add_rect(scr, 0, fy, w, 1, COL_PAGER_ACCENT);
    int ty = fy + (ST_FOOTER_H - TXT_SMALL) / 2;
    if (toolbox_storage_confirm) {
        add_label(scr, 10, ty, TXT_SMALL, COL_RED, "Confirm?  Enter = do it   ESC = cancel");
    } else {
        add_label(scr, 10, ty, TXT_SMALL, COL_HINT, hint);
        add_back_hint(scr, 10 + text_w(hint, TXT_SMALL), ty, back, TXT_SMALL);
    }
}

static const char* const st_cat_name[ST_CAT_COUNT] = {"NVS", "Memory", "Storage", "History", "Backup"};

// The read-only detail views (NVS / Memory / Storage) just paint info rows.
static void render_storage_detail_ro(lv_obj_t* scr, int w, int cat) {
    int  x = 16;
    int  y = ST_HEADER_H + 10;
    char line[112];

    if (cat == ST_CAT_NVS) {
        nvs_stats_t st;
        if (nvs_get_stats(NULL, &st) == ESP_OK) {
            int pct = st.total_entries > 0 ? (int)((st.used_entries * 100) / st.total_entries) : 0;
            snprintf(line, sizeof(line), "%d / %d entries (%d%%) - shared by all apps", (int)st.used_entries,
                     (int)st.total_entries, pct);
        } else {
            snprintf(line, sizeof(line), "stats unavailable");
        }
        add_label(scr, x, y, TXT_BODY, COL_PAGER_TEXT, line);
        y += TXT_BODY + 10;
        add_label(scr, x, y, TXT_SMALL, COL_AMBER, "Per namespace");
        y += TXT_SMALL + 6;
        st_ns_t ns[16];
        int     nns  = st_nvs_ns_counts(ns, 16);
        int     colw = (w - 2 * x) / 2;
        for (int i = 0; i < nns && i < 16; i++) {
            int col  = i % 2;
            int rowy = y + (i / 2) * (TXT_SMALL + 4);
            snprintf(line, sizeof(line), "%-12s %d", ns[i].ns, ns[i].count);
            add_label(scr, x + col * colw, rowy, TXT_SMALL, (i == 0) ? COL_AMBER : COL_GRAY, line);
        }
    } else if (cat == ST_CAT_MEM) {
        unsigned long ram_f = (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
        unsigned long ram_t = (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024);
        unsigned long ps_f  = (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
        unsigned long ps_t  = (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024);
        unsigned long ram_b = (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024);
        snprintf(line, sizeof(line), "RAM:   %lu / %lu KB free", ram_f, ram_t);
        add_label(scr, x, y, TXT_BODY, COL_PAGER_TEXT, line);
        y += TXT_BODY + 6;
        snprintf(line, sizeof(line), "PSRAM: %lu / %lu KB free", ps_f, ps_t);
        add_label(scr, x, y, TXT_BODY, COL_PAGER_TEXT, line);
        y += TXT_BODY + 6;
        snprintf(line, sizeof(line), "Largest free block: %lu KB (internal)", ram_b);
        add_label(scr, x, y, TXT_SMALL, COL_GRAY, line);
    } else {                     // ST_CAT_STORAGE
        static int s_appfs = 0;  // 0 = untried, 1 = ok, -1 = failed
        if (s_appfs == 0) s_appfs = (appfsInit(APPFS_PART_TYPE, APPFS_PART_SUBTYPE) == ESP_OK) ? 1 : -1;
        if (s_appfs == 1)
            snprintf(line, sizeof(line), "AppFS: %lu / %lu KB free", (unsigned long)(appfsGetFreeMem() / 1024),
                     (unsigned long)(appfsGetTotalMem() / 1024));
        else
            snprintf(line, sizeof(line), "AppFS: n/a");
        add_label(scr, x, y, TXT_BODY, COL_PAGER_TEXT, line);
        y               += TXT_BODY + 6;
        uint64_t sd_cap  = history_sd_capacity_bytes();
        char     sd[24];
        if (sd_cap > 0)
            snprintf(sd, sizeof(sd), "%lu MB", (unsigned long)(sd_cap / (1024 * 1024)));
        else
            snprintf(sd, sizeof(sd), "none");
        snprintf(line, sizeof(line), "SD card: %s", sd);
        add_label(scr, x, y, TXT_BODY, COL_PAGER_TEXT, line);
        y       += TXT_BODY + 6;
        int uch  = 0;
        for (int i = 1; i < channel_count && i < CHANNELS_MAX; i++)
            if (channels[i].active) uch++;
        snprintf(line, sizeof(line), "Store: %s (channels + contacts)", locfs_ready() ? "locfd" : "NVS");
        add_label(scr, x, y, TXT_BODY, COL_PAGER_TEXT, line);
        y += TXT_BODY + 6;
        char pk[9];
        for (int i = 0; i < 4; i++) snprintf(pk + i * 2, 3, "%02x", node_pub_key[i]);
        snprintf(line, sizeof(line), "Channels: %d   Contacts: %d   Node: %s...", uch, contact_count, pk);
        add_label(scr, x, y, TXT_SMALL, COL_GRAY, line);
    }
}

// Row layout of the History detail (kept in sync with input.c):
//   [0 .. hn-1]  conversations (delete one)
//   [hn]         "Clear orphaned (N)"   (only when hn > 0)
//   [hn+1]       "Clear all history"    (only when hn > 0)
// When hn == 0 there is a single non-actionable "No stored conversations" row.
static int history_total_rows(int hn) {
    return hn > 0 ? hn + 2 : 1;
}

// History detail: per-conversation size list (cursor-selectable) + bulk-clear
// rows. Destructive rows are confirm-gated via toolbox_storage_confirm.
static void render_storage_detail_history(lv_obj_t* scr, int w, int h) {
    int  x = 16;
    int  y = ST_HEADER_H + 10;
    char line[112];

    history_conv_t convs[40];
    uint64_t       htot = 0;
    int            hn   = history_list_conversations(convs, 40, &htot);

    int orphans = 0;
    for (int i = 0; i < hn; i++)
        if (st_conv_is_orphan(&convs[i])) orphans++;

    char sz[24];
    st_human_size(htot, sz, sizeof(sz));
    snprintf(line, sizeof(line), "Total: %s   (%d conv%s, %d orphan%s)", sz, hn, hn == 1 ? "" : "s", orphans,
             orphans == 1 ? "" : "s");
    add_label(scr, x, y, TXT_SMALL, COL_AMBER, line);
    y += TXT_SMALL + 8;

    int total_rows = history_total_rows(hn);
    if (toolbox_storage_sub < 0) toolbox_storage_sub = 0;
    if (toolbox_storage_sub >= total_rows) toolbox_storage_sub = total_rows - 1;

    int rowh  = ST_ROW_H;
    int avail = (h - ST_FOOTER_H - 6) - y;
    int vis   = avail / (rowh + 4);
    if (vis < 1) vis = 1;
    int start = 0;
    if (toolbox_storage_sub >= vis) start = toolbox_storage_sub - vis + 1;

    int rw = w - 2 * x;
    for (int r = start; r < total_rows && r < start + vis; r++) {
        bool foc = (r == toolbox_storage_sub);
        add_rect(scr, x, y, rw, rowh, foc ? COL_PAGER_ACCENT : COL_PAGER_TILE);
        if (r < hn) {
            char lbl[40];
            st_conv_label(&convs[r], lbl, sizeof(lbl));
            char csz[24];
            st_human_size(convs[r].bytes, csz, sizeof(csz));
            uint32_t col = foc ? COL_HEADER : (st_conv_is_orphan(&convs[r]) ? COL_GRAY : COL_PAGER_TEXT);
            add_label(scr, x + 12, y + (rowh - TXT_BODY) / 2, TXT_BODY, col, lbl);
            add_label(scr, x + rw - 12 - text_w(csz, TXT_SMALL), y + (rowh - TXT_SMALL) / 2, TXT_SMALL,
                      foc ? COL_HEADER : COL_GRAY, csz);
        } else if (hn == 0) {
            add_label(scr, x + 12, y + (rowh - TXT_BODY) / 2, TXT_BODY, COL_GRAY, "No stored conversations");
        } else if (r == hn) {
            char orph[40];
            snprintf(orph, sizeof(orph), "Clear orphaned (%d)", orphans);
            add_label(scr, x + 12, y + (rowh - TXT_BODY) / 2, TXT_BODY, foc ? COL_HEADER : COL_AMBER, orph);
        } else {
            add_label(scr, x + 12, y + (rowh - TXT_BODY) / 2, TXT_BODY, foc ? COL_HEADER : COL_RED,
                      "Clear all history");
        }
        y += rowh + 4;
    }
}

// Backup detail: last-backup date + the backup / restore / factory-reset actions.
static void render_storage_detail_backup(lv_obj_t* scr, int w) {
    int  x = 16;
    int  y = ST_HEADER_H + 10;
    char line[112];

    long bmt       = backup_file_mtime();
    char bdate[24] = "none";
    if (bmt > 0) {
        time_t    t = (time_t)bmt;
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(bdate, sizeof(bdate), "%Y-%m-%d %H:%M", &tmv);
    }
    snprintf(line, sizeof(line), "Last backup: %s", bdate);
    add_label(scr, x, y, TXT_BODY, backup_exists() ? COL_GREEN : COL_PAGER_TEXT, line);
    y += TXT_BODY + 12;

    if (toolbox_storage_sub < 0) toolbox_storage_sub = 0;
    if (toolbox_storage_sub >= STORAGE_ACTION_COUNT) toolbox_storage_sub = STORAGE_ACTION_COUNT - 1;
    int rw = w - 2 * x;
    for (int i = 0; i < STORAGE_ACTION_COUNT; i++) {
        bool foc = (i == toolbox_storage_sub);
        add_rect(scr, x, y, rw, ST_ROW_H, foc ? COL_PAGER_ACCENT : COL_PAGER_TILE);
        add_label(scr, x + 12, y + (ST_ROW_H - TXT_BODY) / 2, TXT_BODY, foc ? COL_HEADER : COL_PAGER_TEXT,
                  storage_actions[i]);
        y += ST_ROW_H + 6;
    }
}

void render_toolbox_storage_lvgl(void) {
    int       w   = (int)lvgl_port_width();
    int       h   = (int)lvgl_port_height();
    lv_obj_t* scr = begin_screen(COL_PAGER_BG);
    pt_reset();

    int detail = toolbox_storage_detail;

    if (detail < 0) {
        // ── Summary: one drill-down row per category ──
        st_chrome(scr, w, h, NULL, "WS: nav   Enter: open   ", ": toolbox");
        int x = 16;
        int y = ST_HEADER_H + 10;
        if (toolbox_storage_cursor < 0) toolbox_storage_cursor = 0;
        if (toolbox_storage_cursor >= ST_CAT_COUNT) toolbox_storage_cursor = ST_CAT_COUNT - 1;
        int         rw   = w - 2 * x;
        const char* open = "> Enter";
        for (int i = 0; i < ST_CAT_COUNT; i++) {
            bool foc = (i == toolbox_storage_cursor);
            add_rect(scr, x, y, rw, ST_ROW_H, foc ? COL_PAGER_ACCENT : COL_PAGER_TILE);
            char val[48];
            st_summary_value(i, val, sizeof(val));
            char line[80];
            snprintf(line, sizeof(line), "%-8s %s", st_cat_name[i], val);
            add_label(scr, x + 12, y + (ST_ROW_H - TXT_BODY) / 2, TXT_BODY, foc ? COL_HEADER : COL_PAGER_TEXT, line);
            add_label(scr, x + rw - 12 - text_w(open, TXT_SMALL), y + (ST_ROW_H - TXT_SMALL) / 2, TXT_SMALL,
                      foc ? COL_HEADER : COL_HINT, open);
            y += ST_ROW_H + 6;
        }
        return;
    }

    // ── Detail views ──
    // Footer hint reflects what Enter does on the focused row, so per-conversation
    // delete is discoverable (vs the single "Clear all history" row).
    const char* hint = "ESC: back   ";
    if (detail == ST_CAT_BACKUP) {
        hint = "WS: nav   Enter: run   ";
    } else if (detail == ST_CAT_HISTORY) {
        history_conv_t tmp[40];
        int            hn    = history_list_conversations(tmp, 40, NULL);
        int            total = history_total_rows(hn);
        int            sub   = toolbox_storage_sub;
        if (sub < 0) sub = 0;
        if (sub >= total) sub = total - 1;
        if (sub < hn)
            hint = "WS: nav   Enter: delete this chat   ";
        else if (hn > 0 && sub == hn)
            hint = "WS: nav   Enter: clear orphaned   ";
        else
            hint = "WS: nav   Enter: clear all   ";
    }
    st_chrome(scr, w, h, st_cat_name[detail], hint, ": storage");

    if (detail == ST_CAT_HISTORY)
        render_storage_detail_history(scr, w, h);
    else if (detail == ST_CAT_BACKUP)
        render_storage_detail_backup(scr, w);
    else
        render_storage_detail_ro(scr, w, detail);
}
