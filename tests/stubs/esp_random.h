// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Host-build stub for esp_random.h so the vendored ed25519.c (which feeds the
// ESP RNG into mbedtls ECP blinding) links in the host test harness. The test
// provides a deterministic esp_fill_random; the ECDH result does not depend on
// the blinding randomness.
#pragma once

#include <stddef.h>

void esp_fill_random(void* buf, size_t len);
