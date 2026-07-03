// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "backup_codec.h"
#include <string.h>

uint32_t backup_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc           = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint16_t get_u16(const uint8_t* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t backup_encode(const backup_data_t* d, uint8_t* buf, size_t cap) {
    if (!d || !buf) return 0;
    if (d->n_channels < 0 || d->n_channels > BACKUP_MAX_CHANNELS) return 0;
    if (d->n_contacts < 0 || d->n_contacts > BACKUP_MAX_CONTACTS) return 0;
    if (d->radio_len > BACKUP_RADIO_MAX) return 0;

    size_t need = BACKUP_HDR_LEN + 1 + (size_t)d->n_channels * (BACKUP_CH_NAME_LEN + BACKUP_SECRET_LEN) + 1 +
                  (size_t)d->n_contacts * (BACKUP_PUB_LEN + BACKUP_ALIAS_LEN + 1) + 1 +
                  (d->have_identity ? BACKUP_SEED_LEN : 0) + 1 + d->radio_len + 1 + (d->have_location ? 8 : 0);
    if (cap < need) return 0;

    memset(buf, 0, need);
    memcpy(buf, BACKUP_MAGIC, 4);
    put_u16(buf + 4, BACKUP_VERSION);
    put_u16(buf + 6, 0);
    // crc (buf+8) filled last, after the body is laid out.

    size_t o = BACKUP_HDR_LEN;
    buf[o++] = (uint8_t)d->n_channels;
    for (int i = 0; i < d->n_channels; i++) {
        memcpy(buf + o, d->channels[i].name, BACKUP_CH_NAME_LEN);
        o += BACKUP_CH_NAME_LEN;
        memcpy(buf + o, d->channels[i].secret, BACKUP_SECRET_LEN);
        o += BACKUP_SECRET_LEN;
    }
    buf[o++] = (uint8_t)d->n_contacts;
    for (int i = 0; i < d->n_contacts; i++) {
        memcpy(buf + o, d->contacts[i].pub, BACKUP_PUB_LEN);
        o += BACKUP_PUB_LEN;
        memcpy(buf + o, d->contacts[i].alias, BACKUP_ALIAS_LEN);
        o        += BACKUP_ALIAS_LEN;
        buf[o++]  = d->contacts[i].role;
    }

    // version-2 tail
    buf[o++] = d->have_identity ? 1 : 0;
    if (d->have_identity) {
        memcpy(buf + o, d->identity_seed, BACKUP_SEED_LEN);
        o += BACKUP_SEED_LEN;
    }
    buf[o++] = d->radio_len;
    if (d->radio_len) {
        memcpy(buf + o, d->radio, d->radio_len);
        o += d->radio_len;
    }
    buf[o++] = d->have_location ? 1 : 0;
    if (d->have_location) {
        put_u32(buf + o, (uint32_t)d->lat_e6);
        o += 4;
        put_u32(buf + o, (uint32_t)d->lon_e6);
        o += 4;
    }

    put_u32(buf + 8, backup_crc32(buf + BACKUP_HDR_LEN, o - BACKUP_HDR_LEN));
    return o;
}

int backup_decode(const uint8_t* buf, size_t len, backup_data_t* out) {
    if (!buf || !out) return BACKUP_ERR_SHORT;
    if (len < BACKUP_HDR_LEN + 2) return BACKUP_ERR_SHORT;  // header + both count bytes minimum
    if (memcmp(buf, BACKUP_MAGIC, 4) != 0) return BACKUP_ERR_MAGIC;
    uint16_t ver = get_u16(buf + 4);
    if (ver != 1 && ver != 2) return BACKUP_ERR_VERSION;
    if (backup_crc32(buf + BACKUP_HDR_LEN, len - BACKUP_HDR_LEN) != get_u32(buf + 8)) return BACKUP_ERR_CRC;

    memset(out, 0, sizeof(*out));
    size_t o = BACKUP_HDR_LEN;

    uint8_t nch = buf[o++];
    if (nch > BACKUP_MAX_CHANNELS) return BACKUP_ERR_RANGE;
    if (o + (size_t)nch * (BACKUP_CH_NAME_LEN + BACKUP_SECRET_LEN) + 1 > len) return BACKUP_ERR_RANGE;
    out->n_channels = nch;
    for (int i = 0; i < nch; i++) {
        memcpy(out->channels[i].name, buf + o, BACKUP_CH_NAME_LEN);
        out->channels[i].name[BACKUP_CH_NAME_LEN - 1]  = '\0';
        o                                             += BACKUP_CH_NAME_LEN;
        memcpy(out->channels[i].secret, buf + o, BACKUP_SECRET_LEN);
        o += BACKUP_SECRET_LEN;
    }

    uint8_t nct = buf[o++];
    if (nct > BACKUP_MAX_CONTACTS) return BACKUP_ERR_RANGE;
    if (o + (size_t)nct * (BACKUP_PUB_LEN + BACKUP_ALIAS_LEN + 1) > len) return BACKUP_ERR_RANGE;
    out->n_contacts = nct;
    for (int i = 0; i < nct; i++) {
        memcpy(out->contacts[i].pub, buf + o, BACKUP_PUB_LEN);
        o += BACKUP_PUB_LEN;
        memcpy(out->contacts[i].alias, buf + o, BACKUP_ALIAS_LEN);
        out->contacts[i].alias[BACKUP_ALIAS_LEN - 1]  = '\0';
        o                                            += BACKUP_ALIAS_LEN;
        out->contacts[i].role                         = buf[o++];
    }

    if (ver == 1) return BACKUP_OK;  // no tail; have_* stay 0 from the memset

    // version-2 tail. A truncated/absent tail is tolerated (treated as absent)
    // so a short-but-CRC-valid v2 file never hard-fails.
    if (o >= len) return BACKUP_OK;
    out->have_identity = buf[o++] ? 1 : 0;
    if (out->have_identity) {
        if (o + BACKUP_SEED_LEN > len) return BACKUP_ERR_RANGE;
        memcpy(out->identity_seed, buf + o, BACKUP_SEED_LEN);
        o += BACKUP_SEED_LEN;
    }
    if (o >= len) return BACKUP_OK;
    out->radio_len = buf[o++];
    if (out->radio_len > BACKUP_RADIO_MAX) return BACKUP_ERR_RANGE;
    if (o + out->radio_len > len) return BACKUP_ERR_RANGE;
    memcpy(out->radio, buf + o, out->radio_len);
    o += out->radio_len;
    if (o >= len) return BACKUP_OK;
    out->have_location = buf[o++] ? 1 : 0;
    if (out->have_location) {
        if (o + 8 > len) return BACKUP_ERR_RANGE;
        out->lat_e6  = (int32_t)get_u32(buf + o);
        o           += 4;
        out->lon_e6  = (int32_t)get_u32(buf + o);
        o           += 4;
    }
    return BACKUP_OK;
}
