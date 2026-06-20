// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "contacts.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"

#define NVS_CONTACTS_BLOB "mc.contacts"

static const char* TAG = "contacts";

contact_t contacts[MAX_CONTACTS];
int       contact_count                = 0;
int       contact_unread[MAX_CONTACTS] = {0};

void contacts_load(void) {
    contact_count = 0;
    memset(contacts, 0, sizeof(contacts));
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READONLY, &handle) != ESP_OK) return;
    size_t blob_sz = sizeof(contacts);
    if (nvs_get_blob(handle, NVS_CONTACTS_BLOB, contacts, &blob_sz) == ESP_OK) {
        int n = (int)(blob_sz / sizeof(contact_t));
        if (n > MAX_CONTACTS) n = MAX_CONTACTS;
        contact_count = n;
        ESP_LOGI(TAG, "Loaded %d contact(s) from NVS", contact_count);
    }
    nvs_close(handle);
}

void contacts_save(void) {
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "NVS open for contacts write failed");
        return;
    }
    if (contact_count == 0) {
        nvs_erase_key(handle, NVS_CONTACTS_BLOB);
    } else {
        nvs_set_blob(handle, NVS_CONTACTS_BLOB, contacts, (size_t)contact_count * sizeof(contact_t));
    }
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Saved %d contact(s) to NVS", contact_count);
}

int contact_find(const uint8_t* pub) {
    for (int i = 0; i < contact_count; i++) {
        if (memcmp(contacts[i].pub_key, pub, MESHCORE_PUB_KEY_SIZE) == 0) return i;
    }
    return -1;
}

// Idempotent add — used to persist anyone we've ever DM'd with.
// Returns 1 if added, 0 if already known, -1 if full.
int contact_ensure(const uint8_t* pub, const char* name, uint8_t role) {
    if (contact_find(pub) >= 0) return 0;
    if (contact_count >= MAX_CONTACTS) return -1;
    int        slot = contact_count;
    contact_t* c    = &contacts[contact_count++];
    memcpy(c->pub_key, pub, MESHCORE_PUB_KEY_SIZE);
    strncpy(c->alias, name ? name : "", CONTACT_ALIAS_LEN - 1);
    c->alias[CONTACT_ALIAS_LEN - 1] = '\0';
    c->role                         = role;
    c->flags                        = 0;
    contact_unread[slot]            = 0;
    contacts_save();
    return 1;
}

// Add (uses node name as alias) or remove. Returns +1 added, 0 removed, -1 full.
int contact_toggle(const uint8_t* pub, const char* name, uint8_t role) {
    int idx = contact_find(pub);
    if (idx >= 0) {
        for (int i = idx; i < contact_count - 1; i++) {
            contacts[i]       = contacts[i + 1];
            contact_unread[i] = contact_unread[i + 1];
        }
        contact_count--;
        memset(&contacts[contact_count], 0, sizeof(contact_t));
        contact_unread[contact_count] = 0;
        contacts_save();
        return 0;
    }
    if (contact_count >= MAX_CONTACTS) return -1;
    int        slot = contact_count;
    contact_t* c    = &contacts[contact_count++];
    memcpy(c->pub_key, pub, MESHCORE_PUB_KEY_SIZE);
    strncpy(c->alias, name ? name : "", CONTACT_ALIAS_LEN - 1);
    c->alias[CONTACT_ALIAS_LEN - 1] = '\0';
    c->role                         = role;
    c->flags                        = 0;
    contact_unread[slot]            = 0;
    contacts_save();
    return 1;
}

void contact_mark_unread(const uint8_t* pub) {
    int idx = contact_find(pub);
    if (idx >= 0) contact_unread[idx]++;
}

void contact_clear_unread(const uint8_t* pub) {
    int idx = contact_find(pub);
    if (idx >= 0) contact_unread[idx] = 0;
}

int contact_unread_total(void) {
    int sum = 0;
    for (int i = 0; i < contact_count; i++) sum += contact_unread[i];
    return sum;
}
