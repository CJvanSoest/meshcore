// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Parse a MeshCore channel share string into a display name + 16-byte secret.
// Accepts either of the two upstream-compatible forms:
//   meshcore://channel/add?name=<url-encoded>&secret=<32 hex>   (full share link)
//   <32 hex>                                                    (bare secret, no name)
//
// On success returns true, writes the URL-decoded name into out_name (truncated
// to name_cap-1, left empty for the bare-hex form) and the 16 raw key bytes into
// out_secret. Returns false for anything that is not one of those two forms
// (e.g. a plain channel name), so callers can fall back to a name-derived
// (community) channel. Channel names carry a leading '#', which the link encodes
// as %23; the decoder restores it.
//
// Pure: no platform dependencies, unit-tested on the host.
bool channel_parse_share(const char* in, char* out_name, size_t name_cap, uint8_t out_secret[16]);
