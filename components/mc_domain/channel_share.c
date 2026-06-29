// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#include "channel_share.h"
#include <string.h>

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Parse exactly 32 hex chars from s into out[16]. Trailing whitespace/newline is
// tolerated; anything else after the 32 chars (or a non-hex char) fails — so a
// plain channel name is rejected and the caller falls back to a community channel.
static bool hex16(const char* s, uint8_t out[16]) {
    while (*s == ' ' || *s == '\t') s++;
    for (int i = 0; i < 16; i++) {
        int hi = hexval(s[2 * i]);
        int lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    const char* rest = s + 32;
    while (*rest == ' ' || *rest == '\t' || *rest == '\r' || *rest == '\n') rest++;
    return *rest == '\0';
}

// Copy the (still-encoded) value of query parameter `key` from query string q
// into buf. Returns the value length, or -1 if the key is absent.
static int query_get(const char* q, const char* key, char* buf, size_t cap) {
    size_t      klen = strlen(key);
    const char* p    = q;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char* v    = p + klen + 1;
            const char* end  = strchr(v, '&');
            size_t      vlen = end ? (size_t)(end - v) : strlen(v);
            if (vlen >= cap) vlen = cap - 1;
            memcpy(buf, v, vlen);
            buf[vlen] = '\0';
            return (int)vlen;
        }
        const char* amp = strchr(p, '&');
        p               = amp ? amp + 1 : NULL;
    }
    return -1;
}

// Decode %XX escapes and '+' (space) in place.
static void url_decode(char* s) {
    char* o = s;
    for (char* p = s; *p;) {
        if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            *o++  = (char)((hexval(p[1]) << 4) | hexval(p[2]));
            p    += 3;
        } else if (*p == '+') {
            *o++ = ' ';
            p++;
        } else {
            *o++ = *p++;
        }
    }
    *o = '\0';
}

bool channel_parse_share(const char* in, char* out_name, size_t name_cap, uint8_t out_secret[16]) {
    if (!in || !out_name || name_cap == 0) return false;
    out_name[0] = '\0';

    static const char PREFIX[] = "meshcore://channel/add?";
    if (strncmp(in, PREFIX, sizeof(PREFIX) - 1) == 0) {
        const char* q = in + (sizeof(PREFIX) - 1);
        char        secbuf[64];
        if (query_get(q, "secret", secbuf, sizeof(secbuf)) < 0) return false;
        if (!hex16(secbuf, out_secret)) return false;
        char namebuf[128];
        if (query_get(q, "name", namebuf, sizeof(namebuf)) >= 0) {
            url_decode(namebuf);
            strncpy(out_name, namebuf, name_cap - 1);
            out_name[name_cap - 1] = '\0';
        }
        return true;
    }

    // Bare 32-hex secret (no name).
    return hex16(in, out_secret);
}
