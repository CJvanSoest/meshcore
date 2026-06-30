# Components and data flow

First-party code is split into `mc_*` ESP-IDF components under `components/`.
The build's `REQUIRES` graph enforces the layer direction, so a backward include
fails to compile rather than just breaking a convention. For the rules see
[`Architecture.md`](Architecture.md), for the design rationale and how to program
here see [`Blueprint.md`](Blueprint.md); this page is the descriptive tour.

## The components

### `mc_common` — foundation (L0)
App-wide constants, the view enum and global UI-state externs, the shared
`gps_/map_` config enums, and the emoji codepoint table (codepoint lookup +
UTF-8 decode). Also the Toolbox **`diag`** ring — a small mutex-protected PSRAM
ring of the most-recent radio frames (both directions), captured straight off
the transport in `radio.c` and read by the packet-log UI. A leaf with no
first-party dependencies, so every higher layer can rely on it.

### `mc_proto` — wire protocol + parsers (L2, pure)
The MeshCore packet/payload codecs (`meshcore/packet.*`, `payload/advert.*`,
`payload/grp_txt.*`), the regulatory tables (`region_limits`), the GPS NMEA
parser, the upstream companion-radio protocol, the Toolbox **`diag_decode`**
packet dissector (+ the `diag_csv_row` CSV formatter for the SD export), and the
**`trace`** reachability-probe codec. No ESP-IDF, FreeRTOS, LVGL or BSP includes
— C stdlib and POSIX only — which is what keeps every file here host-buildable
and unit-tested in `tests/` (`test_diag_decode`, `test_trace`). This is the
upstream protocol mirror: fixes go upstream first, never as local struct growth.

### `mc_crypto` — symmetric crypto (L2)
The channel (GRP_TXT) decrypt/encrypt and the ACK-binding CRC, lifted out of
`radio.c`. mbedtls only, no radio or domain state, so the security-critical
paths are host-tested (`tests/test_mc_crypto.c`): channel-match, ciphertext
tamper rejection, and the ACK CRC. `REQUIRES mc_proto`.

### `vendor` — third-party (leaf)
lodepng, qrcodegen, ed25519 (+ the mbedtls-MPI variant), and the generated
Twemoji bitmaps, kept verbatim from upstream with their own licence headers. Not
first-party: do not restyle it or "fix" its TODOs.

### `mc_io` — platform I/O (L1)
NVS key/value helpers, the GPS UART/I2C reader, and device-certificate
generation. Sits just above the pure cores: it pulls in ESP-IDF drivers but
exposes only small, behaviour-neutral helpers.

### `mc_domain` — application domain (L1/L2)
The stateful core: `identity`, `contacts`, `channels`, `chat`, `nodes`,
`history`, the NVS-backed `settings_nvs`, the settings-driven `sounds` player,
and the Toolbox **`coverage`** test (repeater-reachability result model + TRACE
tag matcher + per-session SD CSV). Mutex-protected in-memory tables plus
persisted config. `sounds` lives
here (not in `mc_net`) so radio and the UI can share it without depending on a
higher layer. Publicly `REQUIRES tanmatsu-lora` because `nodes.h` and
`settings_nvs.h` expose `lora_*` types.

### `mc_radio` — transport (L3)
The LoRa transport: `radio.c`'s send/receive primitives over the P4↔C6 link,
duty-cycle accounting, region scope, and the system-protocol client. RX
deserializes + dedups and hands the raw message to a registered sink; TX is the
`radio_tx_message` tail (region scope + serialize + duty-cycle gate + send). It
builds no MeshCore payloads and touches no domain message state — that lives in
`mc_rx`. Only reads LoRa/region config from `settings_nvs`.

### `mc_net` — connectivity + peripherals (L4)
The HTTPS config server, BLE companion link, serial companion transport, WiFi
keepalive, the GPS task, and the map renderer. These read settings and feed the
domain, so they sit above it.

### `mc_ui` — presentation (L4)
The LVGL 9 UI: the central widget tree (`lvgl_ui.c`) and the display glue
(`lvgl_port.c`, a `flush_cb` → `bsp_display_blit`), the view dispatch
(`render*.c`), input handling, and the emoji picker — including the Toolbox
launcher and its tools (`render_toolbox.c`, `render_toolbox_log.c` for the
packet log, and the coverage test). Custom glyphs and map tiles draw onto an
`lv_canvas`; the screen is rebuilt each frame and input is keyboard-only,
read from the BSP input queue and dispatched by `input.c` (no touch, no LVGL
indev).
Top of the stack: it draws domain state and radio status, and pulls in `lvgl`.

### `mc_rx` — RX application layer (L5)
The MeshCore receive handlers, lifted out of `radio.c` so the radio transport
stays domain-free on receive. `radio.c` deserializes + dedups each packet and
calls the registered sink (`radio_set_rx_sink`); `mc_rx` implements it and owns
the channel/DM decrypt (via `mc_crypto`), the domain writes (chat, contacts,
nodes, channels), the notifications (LED + sounds), and the PATH_RETURN ACK
(sent back through `radio_tx_message`). Registered from `main` at boot; nothing
depends on it (the sink is a callback).

### `main` — app entry (L5)
Just `main.c`: `app_main()`, the boot sequence, and the event loop. Nothing else
belongs in `main/` (enforced by `tests/lint/check-structure.sh`).

## RX flow (a packet arrives)

```
SX1262 → C6 radio → P4 (esp-hosted)
  → mc_radio: lora_rx_task — deserialize (mc_proto) + dedup, then call the sink
  → mc_rx: dispatch by payload type
      GRP_TXT  → mc_crypto: grp_decrypt (try each channel key; MAC = channel-match)
                 → mc_domain: ch_add_message_for + LED/sound
      TXT_MSG  → mc_crypto: dm_decrypt (ed25519 ECDH + AES)
                 → mc_domain: chat_add_dm
                 → mc_rx: dm_send_path_return (ACK, CRC via mc_crypto, sent via radio_tx_message)
      ADVERT   → mc_domain: contacts / nodes
      PATH     → mc_rx: ACK matching against tracked DMs
```

## TX flow (the user sends)

```
mc_ui: input
  → mc_rx: send_chat_message / send_dm_message / send_advert
           (compose from the active channel/contact + identity)
  → mc_crypto: grp_encrypt / dm_encrypt
  → mc_radio: radio_tx_message — region scope + serialize + duty-cycle gate + lora_send
```

The duty-cycle gate and the region-scope clamp are the regulatory guardrails;
both sit in `mc_radio`/`mc_proto` and gate every transmit.
