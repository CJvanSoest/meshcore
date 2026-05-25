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
        ┌─────────────┐         ┌─────────────┐        ┌─────────────┐
        │   input.c   │         │  render.c   │        │   radio.c   │
        │ navigation, │ ──────▶ │ Tokyo Night │        │ LoRa tasks, │
        │ edit-mode   │  state  │ painter for │ ◀───── │ advert,     │
        │ FSM         │         │ every view  │ stats  │ TX/RX rings │
        └─────────────┘         └─────────────┘        └──────┬──────┘
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
