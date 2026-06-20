# Toolbox: Coverage Test (design proposal)

Iteration 2 of the Geeky LoRa Toolbox (#3). **Design only — no code yet.** This
document captures the intended approach so it can be reviewed before
implementation. Iteration 1 (the live packet log, #3 a/b) ships on the
`feature/toolbox-packet-log` branch.

## Goal

Field-test repeater reachability from different positions: pick a discovered
repeater, auto-ping it a few times, classify the result
(green = all OK / orange = partial / red = fail), and log every attempt
(GPS-stamped) to one file on the SD card per session. A later sub-phase adds a
clean map view that drops a colour-coded marker per tested position.

This mirrors the MeshMapper wardriving workflow, but on-badge and offline.

## Reachability primitive (no firmware change)

MeshCore has no native RTT ping. The closest existing primitive is the
**direct message + PATH_RETURN ACK** path already shipped:

- `send_dm_message(text, target_pub, ack_crc_out)` (`mc_rx`) sends a DM and
  returns the 4-byte ACK CRC to watch for.
- `chat_arm_ack_dm(ack_crc)` / `chat_mark_ack_by_crc()` (`mc_domain/chat.c`)
  arm and resolve the ACK; the RX task flips the message's `ack_state` when the
  repeater's PATH_RETURN comes back.

A "ping" = send a fixed-text DM to the repeater's public key, arm the ACK, wait
a bounded window, and check whether `ack_state` flipped. Repeaters are taken
from `node_list[]` filtered on `role == MESHCORE_DEVICE_ROLE_REPEATER`; the
pubkey is enough, the repeater need not be a saved contact.

*Open question for review:* this writes a real DM into the chat ring. Options:
(a) a fixed sentinel text filtered out of the visible DM history, or (b) a
dedicated lightweight ACK-tracking slot kept out of `chat.c`'s user-visible
ring. Preference: (a) for the first cut.

## Flow (sub-phase 2a — list + auto-ping + SD log)

1. New view `VIEW_TOOLBOX_COVERAGE`: scrollable list of discovered repeaters,
   each row showing name + `x/3` ping counter + colour status.
2. On select, spawn a background FreeRTOS task (pattern: `advert_task` in
   `mc_rx`, prio 3) that loops 3×: send ping → arm ACK → wait ~8 s →
   record ACK/no-ACK → `vTaskDelay(10 s)`. UI never blocks.
3. Classify: green = 3/3, orange = 1–2/3, red = 0/3.
4. Append one CSV row per attempt to a single session file
   `/sd/meshcore/coverage/<session>.csv` (`fopen("ab")`, the `history.c`
   pattern), columns:
   `unix_ts, repeater_name, pubkey_prefix, lat_e6, lon_e6, attempt, ack, rtt_ms`.
   GPS from `gps_live_lat_e6/lon_e6` guarded by `gps_live_valid`. One file per
   area test = all positions land in the same log.

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
