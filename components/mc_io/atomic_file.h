// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Crash-safe replacement of a whole file. Writes "<path>.tmp", verifies every
// byte landed, then swaps it into place. On any failure the previous file is
// left untouched, so a full card or a power cut costs the new data rather than
// the old.

#pragma once

#include <stdbool.h>
#include <stddef.h>

// Returns false and leaves `path` as it was if the write or the swap fails.
bool atomic_write_file(const char* path, const void* data, size_t len);
