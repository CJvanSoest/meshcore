// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Pure (no ESP-IDF) serialiser for the on-SD config backup. Kept dependency-free
// so it links into the host test harness. The file-I/O + NVS glue lives in
// backup.c; this translation unit only turns a neutral backup_data_t into a
// versioned, CRC'd byte blob and back.
//
// Wire layout (little-endian), all offsets in bytes:
//   0  magic[4]   = "MCB1"
//   4  u16 version (1 = channels+contacts only; 2 adds the identity/radio/
//                   location tail below)
//   6  u16 reserved = 0
//   8  u32 crc32   (CRC-32/ISO-HDLC over bytes [12..end])
//   12 u8  n_channels
//      n_channels x { char name[24]; u8 secret[16]; }        (40 bytes each)
//      u8  n_contacts
//      n_contacts x { u8 pub[32]; char alias[24]; u8 role; } (57 bytes each)
//   -- version 2 tail --
//      u8  have_identity;  if 1: u8 identity_seed[32]
//      u8  radio_len;      radio_len bytes of opaque lora_cfg
//      u8  have_location;  if 1: i32 lat_e6; i32 lon_e6

#pragma once

#include <stddef.h>
#include <stdint.h>

#define BACKUP_MAGIC   "MCB1"
#define BACKUP_VERSION 2   // encode always writes v2; decode accepts 1 and 2
#define BACKUP_HDR_LEN 12  // magic(4)+ver(2)+rsv(2)+crc(4)

// Mirror the on-device limits (CHANNELS_MAX-1 user channels, MAX_CONTACTS).
#define BACKUP_MAX_CHANNELS 14
#define BACKUP_MAX_CONTACTS 16
#define BACKUP_CH_NAME_LEN  24  // CHANNEL_NAME_MAX_LEN + 1
#define BACKUP_SECRET_LEN   16
#define BACKUP_PUB_LEN      32
#define BACKUP_ALIAS_LEN    24
#define BACKUP_SEED_LEN     32  // Ed25519 identity seed
#define BACKUP_RADIO_MAX    64  // opaque lora_protocol_config_params_t

// Worst-case encoded size, used to size stack/heap buffers on both sides.
#define BACKUP_MAX_BYTES                                                                                              \
    (BACKUP_HDR_LEN + 1 + BACKUP_MAX_CHANNELS * (BACKUP_CH_NAME_LEN + BACKUP_SECRET_LEN) + 1 +                        \
     BACKUP_MAX_CONTACTS * (BACKUP_PUB_LEN + BACKUP_ALIAS_LEN + 1) + 1 + BACKUP_SEED_LEN + 1 + BACKUP_RADIO_MAX + 1 + \
     8)

typedef struct {
    char    name[BACKUP_CH_NAME_LEN];  // NUL-padded
    uint8_t secret[BACKUP_SECRET_LEN];
} backup_channel_t;

typedef struct {
    uint8_t pub[BACKUP_PUB_LEN];
    char    alias[BACKUP_ALIAS_LEN];  // NUL-padded
    uint8_t role;
} backup_contact_t;

typedef struct {
    int              n_channels;
    backup_channel_t channels[BACKUP_MAX_CHANNELS];
    int              n_contacts;
    backup_contact_t contacts[BACKUP_MAX_CONTACTS];
    // version-2 tail (all optional; have_* / *_len == 0 means absent)
    uint8_t          have_identity;
    uint8_t          identity_seed[BACKUP_SEED_LEN];
    uint8_t          radio_len;  // 0..BACKUP_RADIO_MAX
    uint8_t          radio[BACKUP_RADIO_MAX];
    uint8_t          have_location;
    int32_t          lat_e6;
    int32_t          lon_e6;
} backup_data_t;

// backup_decode return codes.
enum {
    BACKUP_OK          = 0,
    BACKUP_ERR_SHORT   = -1,  // buffer smaller than a valid record implies
    BACKUP_ERR_MAGIC   = -2,
    BACKUP_ERR_VERSION = -3,
    BACKUP_ERR_CRC     = -4,
    BACKUP_ERR_RANGE   = -5,  // declared count out of bounds / truncated body
};

// CRC-32/ISO-HDLC (reflected, poly 0xEDB88320) — same as zlib/Ethernet.
uint32_t backup_crc32(const uint8_t* data, size_t len);

// Serialise d into buf (cap bytes) as version 2. Returns encoded length, or 0
// if cap is too small or a count exceeds the BACKUP_MAX_* limits.
size_t backup_encode(const backup_data_t* d, uint8_t* buf, size_t cap);

// Parse buf[0..len) into out. Accepts version 1 (channels+contacts, tail zeroed)
// and version 2. Returns BACKUP_OK (0) or a negative BACKUP_ERR_*.
int backup_decode(const uint8_t* buf, size_t len, backup_data_t* out);
