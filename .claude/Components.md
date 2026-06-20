# Component reference

Per-component map for an AI contributor: what each component owns, what it may
depend on, the key files, and the traps specific to it. This is the "when I
change X, where do I look and what must I not break" reference. The authority on
the layer rules is [docs/architecture/Architecture.md](../docs/architecture/Architecture.md); this file is
the practical index. See [Data-Flows.md](Data-Flows.md) for how they talk at
runtime.

## Layer order and dependency direction

Includes only ever point down. A backward include fails to compile because the
`REQUIRES` graph does not allow it. Layers, low to high:

```
L0  mc_common                      foundation (config, colours, shared defs)
L1  mc_io           mc_domain      platform I/O ; persisted state + app domain
L2  mc_proto  mc_crypto            pure protocol/codec ; symmetric + DM crypto
L3  mc_radio                       LoRa transport (domain free)
L4  mc_net    mc_ui                connectivity ; screens + input
L5  mc_rx     main                 RX handlers + TX composers ; entry point
    vendor                         third-party leaf (imported by many, imports none)
```

`mc_proto` and `vendor` carry no ESP-IDF dependency at the source level for the
pure parts, which is what keeps them host-testable.

## Dependency graph (actual REQUIRES)

| Component | REQUIRES (public) | PRIV_REQUIRES (private) |
|---|---|---|
| `mc_common` | (none) | freertos, esp_hw_support |
| `mc_proto` | (none) | (none) |
| `vendor` | (none) | mbedtls, esp_hw_support |
| `mc_io` | (none) | mc_proto, nvs_flash, esp_driver_i2c, esp_timer, esp_hw_support, mbedtls |
| `mc_domain` | mc_proto, mc_common, tanmatsu-lora, freertos | mc_io, vendor, nvs_flash, mbedtls, fatfs, sdmmc, badge-bsp, tanmatsu-wifi, wifi-manager, lwip, esp_driver_i2s |
| `mc_crypto` | mc_proto | mbedtls, vendor |
| `mc_radio` | freertos, tanmatsu-lora, mc_proto | mc_domain, mc_crypto, (esp-hosted-tanmatsu on P4) |
| `mc_net` | mc_domain, mc_common, badge-bsp, pax-gfx | mc_io, mc_proto, vendor, tanmatsu-lora, esp_http(s)_server, esp_wifi, mdns, wifi-manager, ... |
| `mc_ui` | mc_common, mc_domain, mc_radio, mc_net, pax-gfx, badge-bsp, freertos | mc_io, mc_proto, mc_rx, vendor, wifi-manager, esp_app_format |
| `mc_rx` | (none public) | mc_radio, mc_domain, mc_common, mc_crypto, mc_proto, vendor, mbedtls |

`tanmatsu-lora` is a public REQUIRES of `mc_domain` because a domain header
exposes a LoRa config type. `mc_ui` depends on `mc_rx` privately so screens can
trigger sends; `mc_radio` depends on `mc_domain`/`mc_crypto` privately for
region scope and config persistence, but it never builds a MeshCore payload.

## Constants you will reach for

| Constant | Value | Meaning |
|---|---|---|
| `MAX_NODES` | 200 | node_list capacity (background-discovered peers) |
| `MAX_CONTACTS` | 16 | contacts[] capacity (favourited / DM'd peers) |
| `MESHCORE_PUB_KEY_SIZE` | 32 | ed25519 public key |
| `MESHCORE_SIGNATURE_SIZE` | 64 | ed25519 signature |
| `MESHCORE_MAX_NAME_SIZE` | 32 | advert name field |
| `MESHCORE_CIPHER_KEY_SIZE` | 16 | AES-128 key |
| `MESHCORE_CIPHER_BLOCK_SIZE` | 16 | AES block |
| `MESHCORE_CIPHER_MAC_SIZE` | 2 | on-wire truncated MAC |
| `MESHCORE_MAX_PAYLOAD_SIZE` | 184 | max MeshCore payload body |
| `MESHCORE_MAX_PATH_SIZE` | 64 | max routing path bytes |
| `MESHCORE_MAX_TRANS_UNIT` | 255 | max LoRa frame |

## The components

### `mc_common` (L0)
Foundation defs shared everywhere: `app_config.h` (version labels, board
config), colour palette, small shared types. No logic worth a bug. If you add a
constant used by two or more layers, it may belong here. Also holds `diag.c`,
the diagnostics capture ring (Toolbox packet log): pure transport-log storage
with its own mutex, kept at L0 so the radio tap (`mc_radio`) and the UI both
reach it downward instead of through `mc_domain`. Its display decoder is the
pure `mc_proto/diag_decode`.

### `mc_proto` (L2, pure)
Two parts in one component:
- `meshcore/` is the **upstream protocol mirror** (packet.c, payload/advert.c,
  payload/grp_txt.c). Treat as read-only; fixes go upstream then re-pin. Keep it
  free of ESP-IDF / pax / BSP / L1 headers.
- Root holds **first-party pure helpers**: `region_limits.c` (regulatory
  sub-bands, ERP/EIRP power, duty-cycle budget), `gps_parser.c` (NMEA),
  `advert_sign.c` (the signable-byte range), `diag_decode.c` (the Toolbox
  packet-log frame dissector, kept here so the UI never includes `meshcore/`),
  `companion-radio-protocol/` (the P4<->C6 command parser).
All of it is host-tested. Do not pull a platform header in here.

### `vendor` (leaf)
Third-party drops: lodepng, qrcodegen, ed25519, emoji_bitmaps. **Do not modify.**
Note the ed25519 split documented in [Crypto.md](Crypto.md) and
[Pitfalls.md](Pitfalls.md): `ed25519.c` is X25519 ECDH only, `ed25519_mpi.c` is
the signer. Both ship.

### `mc_io` (L1)
Thin wrappers over ESP-IDF platform APIs: `nvs_helpers.c` (scalar NVS load/save),
`gps.c` (PA1010D over QWIIC), `cert_gen.c` (self-signed cert), SD helpers. Each
should be a shim, not a place for domain logic. Watch uninitialised outputs on a
failed load.

### `mc_domain` (L1/L2)
Persisted state and app data: `settings_nvs.c` (all settings + LoRa config),
`nodes.c` (node_list + the dirty-flag save task), `contacts.c`, `chat.c`
(message history + ACK matching), `channels.c`, `identity.c` (keypair, the boot
crypto self-test, time sync flags), `sounds.c`. **`node_mutex` protects both
`node_list` and `contacts`.** Bounds on the fixed arrays matter here.

### `mc_crypto` (L2)
`mc_crypto.c` (channel GRP_TXT AES-128-ECB + HMAC-SHA256 MAC, ACK CRC, region
transport code) and `mc_crypto_dm.c` (DM ed25519->X25519 ECDH, the 4-variant
HMAC, AES decrypt). Both are pure enough to be host-tested. See [Crypto.md](Crypto.md).

### `mc_radio` (L3)
`radio.c` is **pure transport**: duty-cycle accounting, airtime, RX dedup, the
noise-floor task, the LoRa RX task (deserialize + dedup + hand to the sink),
`radio_tx_message` (the shared TX primitive), region-scope application, config
load/save. It builds no MeshCore payload and decrypts nothing.
`radio_system_protocol_client.c` is the P4<->C6 system-protocol client (a wire
boundary, tracks upstream). Keep `radio.c` domain free.

### `mc_net` (L4)
`http_server.c` (HTTPS test/cert endpoints), `map.c` (slippy-map tile math +
LRU cache + async tile loader). Map tile math and cache indexing are the bug-
prone parts.

### `mc_ui` (L4)
`render_*.c` screens, input handling, view state. `render_settings.c` installs
the `save_*` handlers into a field-dispatch table by address (this is why
cppcheck thinks they are unused, see [Pitfalls.md](Pitfalls.md)). Selection
cursors versus shrinking lists are the classic bug here. Must not include
`meshcore/` — the Toolbox views (`render_toolbox.c` launcher,
`render_toolbox_log.c` packet log) read the `mc_common/diag` ring and the pure
`mc_proto/diag_decode` for display rather than speaking the wire protocol.

### `mc_rx` (L5)
The MeshCore application brain. RX handlers (`rx_handle_advert/grp_txt/dm/path`)
decrypt, write domain state, notify, and ACK. TX composers (`send_advert*`,
`send_dm_message`, `send_chat_message`, the advert task) build payloads and call
`radio_tx_message`. The RX sink is registered with `radio_set_rx_sink` in
`mc_rx_init`. This is where most protocol logic and most of the real bugs live.

### `main`
`main.c` only: the cold-start sequence and the event loop. No first-party logic
goes here. See the cold-start steps in [Data-Flows.md](Data-Flows.md).
