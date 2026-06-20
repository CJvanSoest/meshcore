// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Host-side test for the vendored companion-radio-protocol parser.
// Links against the SAME translation units that ship in the firmware:
//   - mc_companion_command_parser.c
//   - mc_companion_serial_interface.c
//
// Catches regressions in: (a) parse_command extracts opcode + args correctly
// for SET_ADVERT_LATLON, (b) the serial framer reassembles a frame fed in
// chunks of arbitrary size, (c) garbage bytes before '<' are discarded.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "companion-radio-protocol/mc_companion.h"
#include "companion-radio-protocol/mc_companion_command_parser.h"
#include "companion-radio-protocol/mc_companion_serial_interface.h"

static int failures = 0;

#define EXPECT(cond, fmt, ...)                                                           \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

// ── Callback capture ────────────────────────────────────────────────────────
static int                                 cb_count = 0;
static mc_companion_command_parser_error_t cb_err   = COMPANION_COMMAND_PARSER_ERROR_NONE;
static companion_command_packet_t          cb_cmd;

static void capture_cb(companion_command_packet_t* cmd, mc_companion_command_parser_error_t err) {
    cb_count++;
    cb_err = err;
    if (cmd) cb_cmd = *cmd;
}

// ── Frame builder ───────────────────────────────────────────────────────────
// Mirrors scripts/send_latlon.py and the device-side handler: '<' + uint16 LE
// length + payload (opcode + args). Returns total frame length.
static size_t build_latlon_frame(uint8_t out[], int32_t lat_e6, int32_t lon_e6) {
    out[0]               = '<';
    uint16_t payload_len = 1 + 4 + 4;  // opcode + lat + lon
    out[1]               = payload_len & 0xFF;
    out[2]               = (payload_len >> 8) & 0xFF;
    out[3]               = COMPANION_CMD_SET_ADVERT_LATLON;
    memcpy(&out[4], &lat_e6, 4);
    memcpy(&out[8], &lon_e6, 4);
    return 12;
}

int main(void) {
    // ── TV1: full frame in one feed → opcode 14 extracted, lat/lon match. ──
    cb_count = 0;
    cb_err   = COMPANION_COMMAND_PARSER_ERROR_NONE;
    memset(&cb_cmd, 0, sizeof(cb_cmd));

    uint8_t frame[12];
    int32_t exp_lat = 51523083, exp_lon = 5290797;  // somewhere in NL
    size_t  flen = build_latlon_frame(frame, exp_lat, exp_lon);

    mc_companion_read_serial_command(frame, flen, capture_cb);

    EXPECT(cb_count == 1, "callback hit once, got %d", cb_count);
    EXPECT(cb_err == COMPANION_COMMAND_PARSER_ERROR_NONE, "parser err=%d", cb_err);
    EXPECT(cb_cmd.command == COMPANION_CMD_SET_ADVERT_LATLON, "opcode expected 14, got %d", (int)cb_cmd.command);
    EXPECT(cb_cmd.command_set_advert_latlon_args.latitude == exp_lat, "lat expected %d, got %d", exp_lat,
           cb_cmd.command_set_advert_latlon_args.latitude);
    EXPECT(cb_cmd.command_set_advert_latlon_args.longitude == exp_lon, "lon expected %d, got %d", exp_lon,
           cb_cmd.command_set_advert_latlon_args.longitude);

    // ── TV2: garbage prefix before '<' must be silently discarded. ──
    cb_count          = 0;
    uint8_t noisy[20] = {0};
    memcpy(&noisy[0], "JUNK", 4);         // log noise
    memcpy(&noisy[4], "\r\nI(123) ", 9);  // simulated ESP_LOG prefix
    memcpy(&noisy[13], frame, 7);         // partial frame -- not enough to dispatch
    mc_companion_read_serial_command(noisy, 20, capture_cb);
    EXPECT(cb_count == 0, "partial frame should not dispatch yet, got %d hits", cb_count);

    // Feed the rest of the frame; now it should complete.
    mc_companion_read_serial_command(frame + 7, 5, capture_cb);
    EXPECT(cb_count == 1, "completing chunk should dispatch, got %d hits", cb_count);
    EXPECT(cb_cmd.command_set_advert_latlon_args.latitude == exp_lat, "lat after chunked feed expected %d, got %d",
           exp_lat, cb_cmd.command_set_advert_latlon_args.latitude);

    // ── TV3: byte-by-byte feed (worst case for the framer state machine). ──
    cb_count       = 0;
    int32_t bb_lat = -1234567, bb_lon = 9876543;  // negative lat must survive
    flen = build_latlon_frame(frame, bb_lat, bb_lon);
    for (size_t i = 0; i < flen; i++) {
        mc_companion_read_serial_command(&frame[i], 1, capture_cb);
    }
    EXPECT(cb_count == 1, "byte-by-byte feed dispatched %d times (want 1)", cb_count);
    EXPECT(cb_cmd.command_set_advert_latlon_args.latitude == bb_lat, "negative lat survival expected %d, got %d",
           bb_lat, cb_cmd.command_set_advert_latlon_args.latitude);
    EXPECT(cb_cmd.command_set_advert_latlon_args.longitude == bb_lon, "lon byte-by-byte expected %d, got %d", bb_lon,
           cb_cmd.command_set_advert_latlon_args.longitude);

    // ── TV4: parse_command directly with raw payload (opcode + args). ──
    uint8_t payload[9];
    payload[0]         = COMPANION_CMD_SET_ADVERT_LATLON;
    int32_t direct_lat = 42424242, direct_lon = 84848484;
    memcpy(&payload[1], &direct_lat, 4);
    memcpy(&payload[5], &direct_lon, 4);
    companion_command_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    mc_companion_command_parser_error_t err = mc_companion_parse_command(payload, 9, &pkt);
    EXPECT(err == COMPANION_COMMAND_PARSER_ERROR_NONE, "direct parse err=%d", err);
    EXPECT(pkt.command == COMPANION_CMD_SET_ADVERT_LATLON, "direct opcode expected 14, got %d", (int)pkt.command);
    EXPECT(pkt.command_set_advert_latlon_args.latitude == direct_lat, "direct lat expected %d, got %d", direct_lat,
           pkt.command_set_advert_latlon_args.latitude);

    // ── Argument-length + opcode validation (the table that gates every host
    //    command the companion app can issue). ────────────────────────────────
    {
        companion_command_packet_t p;
        uint8_t                    empty[1] = {0};
        EXPECT(mc_companion_parse_command(empty, 0, &p) == COMPANION_COMMAND_PARSER_ERROR_INVALID_COMMAND,
               "empty input is an invalid command");
        uint8_t unknown[1] = {0xEE};
        EXPECT(mc_companion_parse_command(unknown, 1, &p) == COMPANION_COMMAND_PARSER_ERROR_INVALID_COMMAND,
               "undefined opcode is an invalid command");
        uint8_t over[2] = {COMPANION_CMD_GET_DEVICE_TIME, 0x00};
        EXPECT(mc_companion_parse_command(over, 2, &p) == COMPANION_COMMAND_PARSER_ERROR_INVALID_ARGUMENTS,
               "extra args on a zero-arg command is invalid arguments");
        uint8_t under[1] = {COMPANION_CMD_SET_ADVERT_NAME};
        EXPECT(mc_companion_parse_command(under, 1, &p) == COMPANION_COMMAND_PARSER_ERROR_INVALID_ARGUMENTS,
               "too few args on a min-1 command is invalid arguments");
    }

    if (failures == 0) {
        printf("OK -- all companion-protocol test vectors passed\n");
        return 0;
    }
    fprintf(stderr, "%d test vector failure(s)\n", failures);
    return 1;
}
