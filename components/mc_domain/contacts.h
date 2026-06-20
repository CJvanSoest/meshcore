// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "meshcore/packet.h"  // MESHCORE_PUB_KEY_SIZE

#define MAX_CONTACTS      16
#define CONTACT_ALIAS_LEN 24

typedef struct __attribute__((packed)) {
    uint8_t pub_key[MESHCORE_PUB_KEY_SIZE];  // 32
    char    alias[CONTACT_ALIAS_LEN];        // 24, NUL-padded
    uint8_t role;                            // meshcore_device_role_t (1)
    uint8_t flags;                           // reserved (1)
} contact_t;

// Public state — main.c iterates over these for rendering. Mutation goes
// through the contact_*() helpers below so NVS stays in sync.
extern contact_t contacts[MAX_CONTACTS];
extern int       contact_count;

// Per-contact unread DM counter. RAM-only / ephemeral (not persisted to NVS —
// keeping it out of the contact_t blob avoids a format migration). Index-aligned
// with contacts[]; shifted in lockstep whenever a contact is removed.
extern int contact_unread[MAX_CONTACTS];

void contacts_load(void);
void contacts_save(void);

int contact_find(const uint8_t* pub);
int contact_ensure(const uint8_t* pub, const char* name, uint8_t role);
int contact_toggle(const uint8_t* pub, const char* name, uint8_t role);

// Unread helpers (no-op if the pubkey isn't a known contact).
void contact_mark_unread(const uint8_t* pub);   // +1 on the matching slot
void contact_clear_unread(const uint8_t* pub);  // zero the matching slot
int  contact_unread_total(void);                // sum across all contacts
