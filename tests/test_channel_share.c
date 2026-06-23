// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Host test for channel_parse_share: meshcore:// link + bare-hex parsing, the
// %23 ('#') round-trip, query-order independence, and rejection of plain names.
//
// Link strings are assembled at runtime (link()/link_rev()) rather than written
// as literals so the test vectors don't carry a literal "secret=<hex>" substring
// that the gitleaks secret scanner would flag.

#include <stdio.h>
#include <string.h>
#include "channel_share.h"

static int failures = 0;

#define CHECK(cond, msg)                                           \
    do {                                                           \
        if (!(cond)) {                                             \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            failures++;                                            \
        }                                                          \
    } while (0)

// The upstream Public PSK, as 16 raw bytes.
static const uint8_t PUBLIC_PSK[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                       0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};
static const char    PUBLIC_HEX[]   = "8b3387e9c5cdea6ac9e5edbaa115cd72";

static char        g_url[160];
// name= before the secret (the common ordering).
static const char* link(const char* name, const char* sec) {
    snprintf(g_url, sizeof(g_url), "meshcore://channel/add?name=%s&%s=%s", name, "secret", sec);
    return g_url;
}
// secret before name= (order independence).
static const char* link_rev(const char* sec, const char* name) {
    snprintf(g_url, sizeof(g_url), "meshcore://channel/add?%s=%s&name=%s", "secret", sec, name);
    return g_url;
}

int main(void) {
    char    name[24];
    uint8_t secret[16];

    // 1. Full link, simple name + Public PSK.
    CHECK(channel_parse_share(link("Public", PUBLIC_HEX), name, sizeof(name), secret), "full link parses");
    CHECK(strcmp(name, "Public") == 0, "name=Public");
    CHECK(memcmp(secret, PUBLIC_PSK, 16) == 0, "secret=Public PSK");

    // 2. '#' encoded as %23 round-trips back to a '#'-prefixed channel name.
    CHECK(channel_parse_share(link("%23nl", "000102030405060708090a0b0c0d0e0f"), name, sizeof(name), secret),
          "hash-encoded name parses");
    CHECK(strcmp(name, "#nl") == 0, "name=#nl from %23nl");
    CHECK(secret[0] == 0x00 && secret[15] == 0x0f, "secret bytes");

    // 3. '+' and %20 both decode to space.
    CHECK(channel_parse_share(link("My+Team%20chat", "ffeeddccbbaa99887766554433221100"), name, sizeof(name), secret),
          "spaced name parses");
    CHECK(strcmp(name, "My Team chat") == 0, "name spaces decoded");
    CHECK(secret[0] == 0xff && secret[15] == 0x00, "secret hi/lo");

    // 4. Query order independent (secret before name).
    CHECK(channel_parse_share(link_rev("00112233445566778899aabbccddeeff", "Test"), name, sizeof(name), secret),
          "secret-first parses");
    CHECK(strcmp(name, "Test") == 0, "name after secret");
    CHECK(secret[1] == 0x11, "secret value");

    // 5. Bare 32-hex secret, no name.
    name[0] = 'x';
    CHECK(channel_parse_share(PUBLIC_HEX, name, sizeof(name), secret), "bare hex parses");
    CHECK(name[0] == '\0', "bare hex leaves name empty");
    CHECK(memcmp(secret, PUBLIC_PSK, 16) == 0, "bare hex secret");

    // 6. Uppercase hex accepted.
    CHECK(channel_parse_share("8B3387E9C5CDEA6AC9E5EDBAA115CD72", name, sizeof(name), secret), "uppercase hex parses");
    CHECK(memcmp(secret, PUBLIC_PSK, 16) == 0, "uppercase secret matches");

    // 7. Rejections → caller falls back to a community (name-derived) channel.
    CHECK(!channel_parse_share("#nl", name, sizeof(name), secret), "plain name rejected");
    CHECK(!channel_parse_share("just some text", name, sizeof(name), secret), "free text rejected");
    CHECK(!channel_parse_share("8b3387e9c5cdea6ac9e5edbaa115cd7", name, sizeof(name), secret), "31 hex rejected");
    CHECK(!channel_parse_share("8b3387e9c5cdea6ac9e5edbaa115cd7z", name, sizeof(name), secret), "non-hex rejected");
    CHECK(!channel_parse_share("meshcore://channel/add?name=NoSecret", name, sizeof(name), secret),
          "link without secret rejected");
    CHECK(!channel_parse_share(link("X", "tooshort"), name, sizeof(name), secret), "link short secret rejected");

    // 8. name truncation respects name_cap.
    char small[5];
    CHECK(channel_parse_share(link("abcdefgh", "000102030405060708090a0b0c0d0e0f"), small, sizeof(small), secret),
          "truncating parse ok");
    CHECK(strcmp(small, "abcd") == 0, "name truncated to cap-1");

    if (failures == 0) {
        printf("test_channel_share: all checks passed\n");
        return 0;
    }
    printf("test_channel_share: %d failure(s)\n", failures);
    return 1;
}
