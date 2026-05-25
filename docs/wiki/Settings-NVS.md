# Settings / NVS

All persistent state lives in NVS under namespaces declared in
`main/settings_nvs.h` and `main/identity.h` (the `system` namespace is
shared with the launcher for `owner_name`).

## Persistent keys

### `lora` namespace (`settings_nvs.c`)

| Key | Type | Default | Range / values |
|---|---|---|---|
| `freq` | u32 | 869_618_000 Hz | EU 868 ISM band |
| `sf` | u8 | 8 | 7..12 |
| `bw` | u32 | 62 (kHz) | 7, 10, 15, 20, 31, 41, 62, 125, 250, 500 |
| `cr` | u8 | 6 | 5..8 (4/5..4/8) |
| `pwr` | i8 | 22 dBm | -9..22 (chip limit) |
| `sync` | u8 | 0x2B | 0x00..0xFF (0x12 = public LoRa) |
| `preamble` | u16 | 16 | 6..65535 |
| `adv_int` | u32 | 1800 s | 30s..24h |
| `role` | u8 | `CHAT_NODE` | `CHAT_NODE` / `REPEATER` / `ROOM_SERVER` / `SENSOR` |
| `path_h` | u8 | 1 | 1..3 (see protocol page) |

### `system` namespace (shared with launcher)

| Key | Type | Purpose |
|---|---|---|
| `owner_name` | str (≤32) | Owner display name (shared with launcher) |
| `last_time_s` | i64 | Last good SNTP timestamp; restored on boot without WiFi |

### `mc` namespace (MeshCore-specific identity)

| Key | Type | Purpose |
|---|---|---|
| `adv_name` | str (≤32) | Overrides `owner_name` in adverts when non-empty |
| `region` | str (≤8) | Region scope tag (NL, DE, …) |
| `node_pub` | blob 32 | Ed25519 public key |
| `node_prv` | blob 32 | Ed25519 private key |

### `contacts` namespace (`contacts.c`)

| Key | Type | Purpose |
|---|---|---|
| `count` | u8 | Number of saved contacts |
| `c<n>` | blob | Contact n: pub key + alias + role |

`MAX_CONTACTS` = 16; pubkey-prefix collisions are accepted but rare in
practice.

## C6 sync flow

The C6 also holds its own copy of LoRa config. On boot:

1. `load_lora_from_nvs` reads the P4 copy into `lora_cfg`.
2. `lora_get_config(&c6_cfg)` reads the C6 copy.
3. If `c6_cfg.frequency != 0`, the C6 wins → `lora_cfg = c6_cfg` and we
   persist back to NVS.
4. If `c6_cfg.frequency == 0` (blank — typical after a fresh radio flash),
   we push `lora_cfg` to the C6 with `lora_set_config(&lora_cfg)`.

Edits in the Settings tab call `save_lora_to_nvs` *and* `lora_set_config`
immediately on save, so the two copies stay in sync.

## LoRa presets

`settings_nvs.c` carries a `LORA_PRESETS[]` table. Selecting a preset in the
Settings tab overwrites SF, BW, CR (and sync word for the MeshCore preset).
Frequency, TX power, preamble and role are *not* touched by presets.

Default presets shipped:

| Name | SF | BW | CR | Notes |
|---|---|---|---|---|
| MeshCore | 8 | 62 | 4/6 | Matches stock MeshCore default net |
| LongFast | 11 | 250 | 4/5 | Meshtastic-style fast & long-range |
| LongSlow | 12 | 125 | 4/8 | Maximum range, very slow |
| ShortFast | 7 | 500 | 4/5 | Local, high throughput |

When a custom combination is active the Settings row shows `(custom)`.
