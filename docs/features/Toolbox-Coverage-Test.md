# Toolbox: Coverage Test (design + 2a implementation)

Iteration 2 of the Geeky LoRa Toolbox (#3). **Sub-phase 2a (list + auto-ping +
SD log) is implemented on this branch**, stacked on Iteration 1
(`feature/toolbox-packet-log`); sub-phase 2b (the coverage map) is still design
only. Everything is **compile-verified, not yet hardware-tested** — the ping
must be confirmed against a real repeater on the badge.

Implemented in 2a: `mc_domain/coverage.{c,h}` (result model + TRACE-tag matcher +
SD CSV + repeater collector), the ping controller `coverage_ping_start` +
`send_trace` in `mc_rx` with a `rx_handle_trace` hook, and
`VIEW_TOOLBOX_COVERAGE` (`render_toolbox_coverage.c` + input), reached from the
now-enabled "Coverage Test" launcher tile.

## Goal

Field-test repeater reachability from different positions: pick a discovered
repeater, auto-ping it a few times, classify the result
(green = all OK / orange = partial / red = fail), and log every attempt
(GPS-stamped) to one file on the SD card per session. A later sub-phase adds a
clean map view that drops a colour-coded marker per tested position.

This mirrors the MeshMapper wardriving workflow, but on-badge and offline.

## Reachability primitive: TRACE (no firmware change)

The ping is an upstream MeshCore **TRACE** (`PAYLOAD_TYPE_TRACE = 0x09`), not a
DM. A repeater only ACKs a DM (TXT_MSG) from a logged-in admin client
(`examples/simple_repeater/MyMesh.cpp`), so a plain DM+ACK never turns green
against a real repeater (see issue #25). TRACE is handled by every role in the
base `Mesh`, so it is the correct probe.

- `send_trace(target_pub, tag)` (`mc_rx`) builds a TRACE: payload
  `tag[4] | auth[4] | flags[1] | repeater_hash[...]`, DIRECT-routed, on-wire
  `path_length = 0` (the hop hash rides in the payload, per upstream
  `Mesh::sendDirect`). The per-hop SNR path accumulates as it travels.
- The repeater stamps its RX SNR into the path field and rebroadcasts; the
  returning frame carries our random `tag`, so `rx_handle_trace` recognises it
  and calls `coverage_note_tag(tag, uplink_snr, downlink_snr)`.
- `uplink_snr` = the SNR the repeater measured receiving from us (path[0]);
  `downlink_snr` = our SNR of the returning frame. Both go in the CSV.

TRACE is exempted from the RX dedup (`radio.c`): its payload is constant while
only the SNR path changes, so the payload-fingerprint dedup would otherwise drop
the return. Repeaters are taken from `node_list[]` filtered on
`role == MESHCORE_DEVICE_ROLE_REPEATER`; the pubkey prefix is the hop hash.

The wire payload layout is the pure, host-tested `mc_proto/trace.{c,h}`
(`test_trace`); the TRACE envelope reuses the normal `meshcore_serialize`.

## Flow (sub-phase 2a — list + auto-ping + SD log)

1. New view `VIEW_TOOLBOX_COVERAGE`: scrollable list of discovered repeaters,
   each row showing name + `x/3` ping counter + colour status.
2. On select, spawn a background FreeRTOS task (pattern: `advert_task` in
   `mc_rx`, prio 3) that loops 3×: pick a random tag → `send_trace` → arm the
   tag → wait ~8 s for the return → record reachable/SNR → `vTaskDelay(10 s)`.
   UI never blocks.
3. Classify: green = 3/3, orange = 1–2/3, red = 0/3.
4. Append one CSV row per attempt to a single session file
   `/sd/meshcore/coverage/cov_<unix>.csv`, columns:
   `ts_unix, repeater, pubkey, lat_e6, lon_e6, attempt, reachable, rtt_ms,
   uplink_snr_db, downlink_snr_db`. GPS from `gps_live_lat_e6/lon_e6` guarded by
   `gps_live_valid`. One file per area test = all positions land in the same log.

## Coverage map (sub-phase 2b — later)

Kept **separate** from the existing `VIEW_MAP` (which draws node/repeater pins),
so the coverage map stays clean:

- Dedicated `VIEW_COVERAGE_MAP` with its **own** centre/zoom state (don't mutate
  `map_center_*` / `map_zoom`), zoom locked to 15–16. Reuse the tile cache +
  loader (`map_tile_get`, `render_tile_raster` logic) for the base raster, but
  render **only** coverage markers — skip `render_node_pins`.
- Markers: small in-RAM array of `{lat_e6, lon_e6, result}` (also persisted in
  the CSV so a session can be replayed). Project with `latlon_to_fb()`, draw a
  colour-coded dot via `pax_simple_circle`.
- "Save test area as one PNG": render the bounding box of all markers into an
  offscreen `pax_buf_t`, encode once to `/sd/meshcore/coverage/<session>.png`.
  Note: the vendored lodepng is **decoder-only** today
  (`-DLODEPNG_NO_COMPILE_ENCODER`, `components/vendor/CMakeLists.txt`). Either
  re-enable the encoder (costs flash) or write an uncompressed BMP. Preference:
  re-enable the encoder; document the size delta in the PR.

## Out of scope

- Noise-floor scan (#3c) — needs C6 radio firmware (RSSI sweep).
- True RTT timing beyond the DM/ACK round-trip estimate.

Refs #3.
