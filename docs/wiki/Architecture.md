# Architecture

The app is a single ESP-IDF firmware image for the ESP32-P4 (Tanmatsu app
processor). Everything runs in one process; concurrency comes from a handful
of FreeRTOS tasks plus the main event loop.

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
        │ heard table │  │ DM + chan   │  │ Ed25519 KP, │  │ favourites  │
        │ + filter    │  │ rings + LED │  │ SNTP        │  │ in NVS      │
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

## Tasks and synchronisation

| Task | Created by | Purpose |
|---|---|---|
| `app_main` | IDF | Boot DIAG, event loop, render dispatch |
| `lora_rx_task` | `radio_start_tasks` | Reads packets from C6, dispatches to chat/nodes |
| `advert_task` | `radio_start_tasks` | Periodic ADVERT TX based on `advert_interval_s` |
| `sntp_task` (idf) | `esp_sntp_init` | NTP polling once WiFi connects |

Shared mutable state is protected by:

- `node_mutex` — node and contact tables
- `chat_mutex` — DM ring buffer + DM target
- `ch_mutex` — channel ring buffer
- `rx_mutex` — RX counter
- `s_mutex` (in `history.c`) — SD-card file access

## Cold-start sequence

1. NVS init (erase + retry if version mismatch)
2. BSP init (display, input, power, LED)
3. `nodes_init`, `chat_init`, `identity_init` (creates mutexes + loads Ed25519 keys)
4. Boot DIAG screen begins drawing
5. WiFi stack init → `wifi_connect_try_all` → on success start SNTP
6. NVS time restore if SNTP didn't sync
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

Cross-file declarations (the per-view entry points + a few shared helpers
like `render_msg_list`, the category bounds API, the home tile API)
live in `render_internal.h`. The public API in `render.h` stays small —
palette macros, layout constants, `render()`, `blit()`.

Drawing model: each `render_*()` writes into the framebuffer but does NOT
blit. The dispatcher calls one (and optionally an overlay on top), then
blits exactly once at the end. This is what kills the v2.1.x QR + emoji
overlay flicker.

The render split also gave us a natural place to grow without touching
the existing views — a future `render_map.c` for the Map tile, for
example, drops in without re-flowing any other file.
