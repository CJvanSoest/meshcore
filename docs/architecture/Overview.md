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
        │   input.c   │         │   render.c +    │    │   radio.c   │
        │ navigation, │ ──────▶ │   render_*.c    │    │ LoRa tasks, │
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
+ `ble_companion.c` (companion-radio link), `http_server.c` + `cert_gen.c`
(on-device HTTPS `/ping`), `wifi_keepalive.c`, `sounds.c`, `map.c` (slippy-map
tiles), `emoji.c`, `channels.c`, and `radio_system_protocol_client.c`.

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

## Render split (v2.2.0)

`render.c` used to be a 1.3-k-line monolith that painted every view. As of
v2.2.0 it's a thin dispatcher plus the shared Pager status strip and the
emoji-picker overlay; each view lives in its own file:

| File | Owns |
|---|---|
| `render.c` | Dispatcher (`render()`), shared `render_tab_bar` (the Pager strip), `render_emoji_picker_overlay`, `blit()`, global framebuffer `fb` |
| `render_home.c` | `VIEW_HOME` tile grid, 8 home tiles + icons, status toast |
| `render_settings.c` | Category tile grid + drilldown row list, 6 category icons, the field rows table |
| `render_nodes.c` | `VIEW_NODES` table + `render_qr_overlay` (only triggered from this view or the QR home tile) |
| `render_chat.c` | `VIEW_CHAT` (DM inbox + conversation) + shared `render_msg_list` + word-wrap |
| `render_channel.c` | `VIEW_CHANNEL` (list mode + conversation) |
| `render_about.c` | `VIEW_ABOUT` (version, author, credits, license, source) |
| `render_map.c` | `VIEW_MAP` (slippy-map tiles, node markers, GPS centre) |
| `render_settings_icons.c` | The Settings category glyphs, kept out of `render_settings.c` |

Cross-file declarations (the per-view entry points + a few shared helpers
like `render_msg_list`, the category bounds API, the home tile API)
live in `render_internal.h`. The public API in `render.h` stays small —
palette macros, layout constants, `render()`, `blit()`.

Drawing model: each `render_*()` writes into the framebuffer but does NOT
blit. The dispatcher calls one (and optionally an overlay on top), then
blits exactly once at the end. This is what kills the v2.1.x QR + emoji
overlay flicker.

The render split also gives a natural place to grow without touching the
existing views: `render_map.c` (the Map tile) and `render_settings_icons.c`
both dropped in this way without re-flowing any other view file.
