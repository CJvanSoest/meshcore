// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// ICMP keepalive that pings the WiFi gateway every few seconds so iOS
// Personal Hotspot doesn't suspend the link when the iPhone is idle.
// iPhone drops connected stations after ~10 s of no observed traffic --
// a periodic ping is enough activity to keep the hotspot up.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// Start pinging the default gateway every interval_ms. Stops any previous
// session first. Safe to call from any task. Returns true on success.
bool wifi_keepalive_start(uint32_t interval_ms);

// Stop the active ping session. No-op if none.
void wifi_keepalive_stop(void);

// Spawn a background poll task that auto-starts the ping when WiFi comes up
// and stops it on disconnect. Survives suspend/resume cycles of the link.
// Call once at boot, after wifi_connection_init_stack().
void wifi_keepalive_supervisor_start(void);
