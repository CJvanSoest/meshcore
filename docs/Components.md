# Components and data flow

First-party code is split into `mc_*` ESP-IDF components under `components/`.
The build's `REQUIRES` graph enforces the layer direction, so a backward include
fails to compile rather than just breaking a convention. For the rules see
[`../ARCHITECTURE.md`](ARCHITECTURE.md); this page is the descriptive tour.

## The components

### `mc_common` — foundation (L0)
App-wide constants, the view enum and global UI-state externs, the shared
`gps_/map_` config enums, and the pax-free emoji table (codepoint lookup +
UTF-8 decode). A leaf with no first-party dependencies, so every higher layer
can rely on it.

### `mc_proto` — wire protocol + parsers (L2, pure)
The MeshCore packet/payload codecs (`meshcore/packet.*`, `payload/advert.*`,
`payload/grp_txt.*`), the regulatory tables (`region_limits`), the GPS NMEA
parser, and the upstream companion-radio protocol. No ESP-IDF, FreeRTOS, pax or
BSP includes — C stdlib and POSIX only — which is what keeps every file here
host-buildable and unit-tested in `tests/`. This is the upstream protocol
mirror: fixes go upstream first, never as local struct growth.

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
`history`, the NVS-backed `settings_nvs`, and the settings-driven `sounds`
player. Mutex-protected in-memory tables plus persisted config. `sounds` lives
here (not in `mc_net`) so radio and the UI can share it without depending on a
higher layer. Publicly `REQUIRES tanmatsu-lora` because `nodes.h` and
`settings_nvs.h` expose `lora_*` types.

### `mc_radio` — comm bridge (L3)
The LoRa RX/TX engine (`radio.c`) and the P4↔C6 system-protocol client. RX
handling decodes a packet via `mc_proto`, decrypts via `mc_crypto`, and writes
straight into `mc_domain` (chat/contacts/nodes); TX composes the reverse. It
`REQUIRES mc_domain`, which is a legal downward edge.

### `mc_net` — connectivity + peripherals (L4)
The HTTPS config server, BLE companion link, serial companion transport, WiFi
keepalive, the GPS task, and the map renderer. These read settings and feed the
domain, so they sit above it.

### `mc_ui` — presentation (L4)
The pax-based renderers (`render*.c`), input handling, and the emoji picker. Top
of the stack: it draws domain state and radio status, and pulls in `pax-gfx`.

### `main` — app entry (L5)
Just `main.c`: `app_main()`, the boot sequence, and the event loop. Nothing else
belongs in `main/` (enforced by `tests/check-structure.sh`).

## RX flow (a packet arrives)

```
SX1262 → C6 radio → P4 (esp-hosted)
  → mc_radio: lora_rx_task
  → mc_proto: meshcore_deserialize        (frame → typed message)
  → by payload type:
      GRP_TXT  → mc_crypto: grp_decrypt (try each channel key; MAC = channel-match)
                 → mc_domain: ch_add_message_for + LED/sound
      TXT_MSG  → mc_radio: dm_decrypt (ed25519 ECDH + AES)
                 → mc_domain: chat_add_message
                 → mc_radio: dm_send_path_return (ACK, CRC via mc_crypto)
      ADVERT   → mc_domain: contacts / nodes
      PATH     → mc_radio: ACK matching against tracked DMs
```

## TX flow (the user sends)

```
mc_ui: input
  → mc_domain: compose (active channel / contact, identity)
  → mc_radio: send_chat_message / send_dm_message
  → mc_crypto: grp_encrypt / DM encrypt
  → mc_proto: meshcore_serialize + apply region scope (region_limits)
  → mc_radio: duty-cycle budget gate
  → C6 radio: lora_send_packet
```

The duty-cycle gate and the region-scope clamp are the regulatory guardrails;
both sit in `mc_radio`/`mc_proto` and gate every transmit.
