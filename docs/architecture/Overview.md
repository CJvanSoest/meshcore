# Architecture

The app is a single ESP-IDF firmware image for the ESP32-P4 (Tanmatsu app
processor). Everything runs in one process; concurrency comes from a handful
of FreeRTOS tasks plus the main event loop.

> For the layer discipline (L0–L5), the forbidden-include rules and the
> wire-boundary rules, see [Architecture.md](Architecture.md). For why the code
> is shaped this way and how to program within it, see [Blueprint.md](Blueprint.md).
> First-party code is split into `mc_*` components under `components/`
> (protocol core in `mc_proto`, channel crypto in `mc_crypto`, third-party in
> `vendor`); `main` is just the entry point.
> This page is the descriptive tour; Architecture.md is the rulebook.

For the build-level view — which `mc_*` component each module lives in, the
`REQUIRES` dependency graph, and the RX/TX data flow — see
[Components.md](Components.md). This page is the runtime/module tour.

## Module overview

```
                            ┌──────────────────────┐
                            │       main.c         │
                            │  app_main(),         │
                            │  boot DIAG, event    │
                            │  loop                │
                            └──────────┬───────────┘
                                       │ dispatches
                ┌──────────────────────┼──────────────────────┐
                ▼                      ▼                      ▼
        ┌─────────────┐         ┌─────────────────┐    ┌─────────────┐
        │   input.c   │         │   lvgl_ui.c +   │    │   radio.c   │
        │ navigation, │ ──────▶ │  lvgl_<view>.c  │    │ LoRa tasks, │
        │ edit-mode   │  state  │  per-view +     │ ◀──│ advert,     │
        │ FSM         │         │ Pager strip     │ stats │ TX/RX rings │
        └─────────────┘         └─────────────────┘    └──────┬──────┘
                                                              │ frames
                ┌─────────────────────────────────────────────┘
                ▼
        ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐
        │   nodes.c   │  │   chat.c    │  │ identity.c  │  │ contacts.c  │
        │ heard table │  │ DM + chan   │  │ Ed25519 KP  │  │ favourites  │
        │ + filter    │  │ rings + LED │  │ (time: RTC) │  │ in NVS      │
        └─────────────┘  └──────┬──────┘  └─────────────┘  └─────────────┘
                                │
                                ▼
                       ┌─────────────────┐  ┌─────────────────┐
                       │   history.c     │  │ settings_nvs.c  │
                       │ SD-card mount,  │  │ LoRa config +   │
                       │ AES-CBC append/ │  │ names + presets │
                       │ load, self-heal │  │ in NVS          │
                       └─────────────────┘  └─────────────────┘
```

The diagram is the core message path. Several subsystems are left out to keep
it readable: `gps.c`/`gps_task.c` (PA1010D + live fix), `companion_transport.c`
+ `ble_companion.c` (companion-radio link), `wifi_keepalive.c`, `sounds.c`,
`map.c` (slippy-map tiles), `emoji.c`, `channels.c`, and
`radio_system_protocol_client.c`.

## Tasks and synchronisation

About a dozen FreeRTOS tasks run beside the `app_main` event loop. The main
ones:

| Task | Created by | Purpose |
|---|---|---|
| `app_main` | IDF | Boot DIAG, event loop, render dispatch |
| `lora_rx` | `radio_start_tasks` | Reads packets from C6, dedups floods, dispatches ADVERT→nodes, text→chat, ACK/PATH handling |
| `lora_advert` | `radio_start_tasks` | Periodic ADVERT TX based on `advert_interval_s` |
| `noise_poll` | `radio_start_tasks` | Polls the ambient noise floor (NACK-tolerant for old C6) |
| `gps_task` | `gps_task_start` | Polls the PA1010D and publishes the live fix |
| `comp-tx` | `companion_transport_init` | Reads USB-CDC stdin into the companion parser |
| `nodes_save` | `nodes_start_save_task` | Saves the node table to SD when dirty (~30 s) |
| `map_loader` | `map_loader_init` | Background SD → PNG → RGB565 tile decode |
| `sound_play` | `sounds_play_*` | One-shot tone/WAV playback |
| `cov_ping` | `coverage_ping_start` | Transient: pings a repeater 3x for the Toolbox coverage test, then exits |
| `wifi-ka-sup` | `wifi_keepalive_supervisor_start` | ICMP keepalive on link up/down |
| NimBLE host | `ble_companion_init` | NimBLE GATT stack (when BLE is enabled) |

Shared mutable state is protected by per-table mutexes; lock ordering is by
convention, so hold the right lock when touching each table:

- `node_mutex` — node table **and** the contacts table (note this coupling:
  `send_advert_direct` walks `contacts[]` under `node_mutex`)
- `chat_mutex` — DM ring buffer + DM target
- `ch_mutex` — channel ring buffer
- `rx_mutex` — raw RX debug ring + counter
- `s_mutex` (in `history.c`) — SD-card file access
- `s_cache_mutex` + a loader queue (in `map.c`) — tile cache + loader requests
- `s_dispatch_mutex` (in `companion_transport.c`) — serialises BLE/CDC feeds
  into the single companion parser

GPS uses a `portMUX` critical section rather than a mutex. There are no event
groups.

## Cold-start sequence

1. NVS init (erase + retry if version mismatch)
2. BSP init (display, input, power, LED)
3. `nodes_init`, `chat_init`, `identity_init` (creates mutexes + loads Ed25519 keys)
4. Boot DIAG screen begins drawing
5. WiFi stack init (brings up the P4↔C6 link; it does **not** connect or scan)
6. Time comes from the C6 RTC (`bsp_rtc_update_time`); NVS time restore as
   fallback. There is no in-app SNTP path.
7. Owner / advert / region / contacts load from NVS
8. `history_init(node_prv_key)` → mounts SD, derives AES-CBC key from identity
9. `lora_init(16)` → talks to the C6
10. `load_lora_from_nvs` then `lora_get_config` from C6:
    - If C6 has a real config, prefer it and persist back to NVS
    - If C6 is blank (`frequency=0`), push NVS config to C6
11. `lora_set_mode(RX)` + `radio_start_tasks`
12. `render()` and enter event loop

After the boot phase the only periodic work happens through:

- Input events from the BSP queue (≤ 1 s wait, render after each)
- LoRa RX task pushing frames into `chat`/`nodes`
- Advert task firing on its interval

## UI file split

The UI has been split twice. In v2.2.0 the PAX painter came out of one 1.3k line
`render.c` into a file per view. The LVGL 9 migration then collapsed those views
back into a single `lvgl_ui.c`, which reached 3790 lines before being split again
along the same lines.

| File | Owns |
|---|---|
| `lvgl_ui.c` | Lifecycle and the per-view dispatch (`lvgl_view_render`) |
| `lvgl_common.c` | Screen and widget primitives, boot splash, vector glyphs, tab bar, status toast |
| `lvgl_internal.h` | The declarations the view files share; not part of the public API |
| `lvgl_home.c` | `VIEW_HOME` tile grid |
| `lvgl_settings.c` | `VIEW_SETTINGS` category grid, drilldown and the category glyphs |
| `lvgl_nodes.c` | `VIEW_NODES` list |
| `lvgl_chat.c` | `VIEW_CHAT` plus the shared message ring renderer and word wrap |
| `lvgl_channel.c` | `VIEW_CHANNEL` list, wizard and conversation |
| `lvgl_map.c` | `VIEW_MAP` tile canvas and GPS overlay |
| `lvgl_toolbox.c` | `VIEW_TOOLBOX` launcher |
| `lvgl_log.c` | `VIEW_TOOLBOX_LOG` packet log, list and detail |
| `lvgl_coverage.c` | `VIEW_TOOLBOX_COVERAGE` repeater coverage test |
| `lvgl_storage.c` | `VIEW_TOOLBOX_STORAGE` storage viewer |
| `lvgl_about.c` | `VIEW_ABOUT` |
| `lvgl_ble.c` | `VIEW_BLE_DEVICES` paired-device list |
| `lvgl_emoji.c` | Inline emoji and the emoji picker overlay |
| `lvgl_qr.c` | The QR overlay (contact / channel / OwnTracks) |
| `lvgl_port.c` | Display glue: `flush_cb` into `bsp_display_blit` |
| `render.c` / `render.h` | Panel geometry, the `COL_*` palette and `TXT_*` sizes |

Four files kept a `render_` name through the LVGL migration while holding no
rendering at all. They are now named for what they are: `home_tiles.c`,
`settings_fields.c`, `toolbox_tiles.c` and `packet_log.c`. Each is a single
source of truth (tile metadata, the settings field table and save dispatch, the
packet-log export and formatters) that the matching LVGL view reads through the
accessors in `render_internal.h`.

Drawing model: LVGL owns the panel. Each `render_<view>_lvgl()` rebuilds the
screen's widget tree and `lvgl_port.c`'s `flush_cb` blits it. Input is keyboard
only, read from the BSP input queue and dispatched by `input.c`; there is no
LVGL indev.
