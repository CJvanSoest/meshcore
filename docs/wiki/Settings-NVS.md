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
| `country` | str(4) | `"--"` | ISO 3166-1 alpha-2; `"--"` = none (no checks) |
| `antgain` | i8 | 0 dBi | -3..15; editable only once `country` ≠ `"--"` |
| `rxboost` | u8 | 1 (on) | 1 = boosted RX (+3 dB sensitivity), 0 = power-save |
| `gps.lat` | i32 | — | Latitude ×1e6 (advert position + Nodes Dist column) |
| `gps.lon` | i32 | — | Longitude ×1e6 |

> All of the above live in the single `system` NVS namespace under dotted
> key names (`lora.freq`, `lora.country`, …); the `lora.` prefix is logical,
> not a separate namespace.

### `system` namespace (shared with launcher)

| Key | Type | Purpose |
|---|---|---|
| `owner_name` | str (≤32) | Owner display name (shared with launcher) |
| `last_time_s` | i64 | Last good SNTP timestamp; restored on boot without WiFi |

### `ui` keys (per-app brightness, in `system` namespace)

Per-app overrides for the three BSP backlight / LED globals. Applied on
boot by `apply_brightness()` and again on every value change in the
Brightness settings category, so the launcher's globals stay untouched
once MeshCore is running. All three are `u8`, range 0..100 %, defaults
chosen to match the backlog #47 trade-off (battery vs. visibility).

| Key | Type | Default | BSP call |
|---|---|---|---|
| `ui.disp_bl` | u8 | 50 | `bsp_display_set_backlight_brightness(pct)` |
| `ui.kb_bl` | u8 | 50 | `bsp_input_set_backlight_brightness(pct)` |
| `ui.led_br` | u8 | 5 | `bsp_led_set_brightness(pct)` |

Edited via Settings → Brightness; the slider cycles through
5 / 10 / 25 / 50 / 75 / 100 % stops. The launcher's globals are NOT
restored on exit — BadgeVMS PIE ELF apps have no clean exit hook, so the
MeshCore value persists until the launcher writes its own value next
time the launcher's brightness UI runs.

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

## Regulatory compliance

`main/region_limits.{h,c}` carries a per-country table of the licence-exempt
ISM-band rules. Picking a **Country** in the Settings tab drives three things:
off-band / over-power warnings, the antenna-gain → ERP/EIRP calculation, and a
hard duty-cycle budget enforced in `radio.c`.

### Enforcement model

| Limit | Mode | Where |
|---|---|---|
| Frequency outside any sub-band | **soft** — Country/Frequency rows turn red, footer hint | `render.c` |
| Effective power > sub-band max | **soft** — TX power row turns red | `render.c` (`region_effective_power_dbm`) |
| Duty cycle (rolling 1 h airtime) | **hard** — TX blocked when budget spent | `radio.c` (Semtech time-on-air) |

The split is deliberate: a wrong frequency or a few dB over is the operator's
call to make (they may have a licence, or a directional antenna), but airtime
hogging actively degrades the shared mesh, so the duty-cycle cap is enforced.

> The data table is a guidance helper, **not legal advice**. It is also an
> interim app-side implementation — radio-side enforcement may move into the
> C6 firmware once upstream `tanmatsu-radio` grows a compliance layer.

### Data schema

```c
typedef struct {
    float    freq_min_mhz, freq_max_mhz;
    int8_t   max_power_dbm;        // unit per containing country (ERP or EIRP)
    uint16_t duty_cycle_permille;  // 1000 = none, 100 = 10%, 10 = 1%, 1 = 0.1%
    bool     lbt_alternative;      // LBT may replace duty cycle (informational)
    uint16_t max_dwell_time_ms;    // 0 = none (FCC FHSS = 400)
    const char *label;             // e.g. "g3"
} regulatory_subband_t;

typedef struct {
    const char *country_code;      // ISO 3166-1 alpha-2
    const char *display_name;
    power_unit_t power_unit;       // POWER_UNIT_ERP | POWER_UNIT_EIRP
    const regulatory_subband_t *subbands;
    uint8_t n_subbands;
} regulatory_country_t;
```

### ERP vs EIRP

The EU regulates in **ERP** (dipole reference); the US/JP/AU/NZ/etc in **EIRP**
(isotropic). The app converts your conducted TX power to the country's unit
before comparing against the limit:

```
ERP  = conducted_dBm + antenna_gain_dBi − 2.15
EIRP = conducted_dBm + antenna_gain_dBi
```

So at the SX1262's 22 dBm conducted ceiling with a 0 dBi antenna you radiate
~19.85 dBm ERP — comfortably under the EU g3 limit of 27 dBm ERP.

### EU 863–870 MHz sub-bands (ERP) — `EU868_SUBBANDS`

| Band | Range (MHz) | Max power | Duty cycle |
|---|---|---|---|
| g | 863.0–865.0 | 14 dBm | 0.1% |
| g1 | 865.0–868.0 | 14 dBm | 1% |
| g1' | 868.0–868.6 | 14 dBm | 1% |
| g2 | 868.7–869.2 | 14 dBm | 0.1% |
| **g3** | **869.4–869.65** | **27 dBm** | **10%** |
| g4 | 869.7–870.0 | 14 dBm | 1% |

Used by: NL, BE, DE, AT, FR, CH, UK, IT, ES, PT, SE, NO, DK, FI, PL, CZ, IE,
UA, ZA (19 countries). Default **869.618 MHz → g3**.

### Other regions

| Constant | Countries | Range (MHz) | Max power | Duty / dwell |
|---|---|---|---|---|
| `EU433_SUBBANDS` | 433 SRD | 433.05–434.79 | 10 dBm ERP | 10% |
| `US915_SUBBANDS` | US, CA, MX | 902–928 | 30 dBm EIRP | none (FHSS dwell 400 ms info) |
| `ANZ915_SUBBANDS` | AU, NZ | 915–928 | 30 dBm EIRP | none |
| `JP920_SUBBANDS` | JP | 920.5–923.5 | 13 dBm EIRP | 10% + LBT required |
| `KR920_SUBBANDS` | KR | 920.0–923.0 | 14 dBm EIRP | 10% |
| `IN865_SUBBANDS` | IN | 865.0–867.0 | 30 dBm EIRP | none |
| `RU864_SUBBANDS` | RU | 864–865 / 868.7–869.2 | 14 dBm ERP | 0.1% |

### Duty-cycle accounting

`radio.c` keeps 60 one-minute airtime buckets (rolling hour). Every TX path
estimates on-air time with the Semtech LoRa time-on-air formula (AN1200.13)
and checks `dc_budget_available()` before sending; `region_dc_budget_ms_per_hour`
turns the sub-band's permille into the ms/hour budget (10% → 360 000 ms). The
Settings *Duty cycle (1h)* row surfaces `used.x% (used s / budget s)` and
appends `BLOCKED` when a send was just refused.

### Sources

- EU 863–870: ETSI EN 300 220 V3.2.1 + ERC-REC-70-03
- US 902–928: FCC Part 15.247 / 15.249
- Country→band mapping: Lansitec LoRaWAN frequency-plan reference
- Cross-check only: Meshtastic `src/mesh/RadioInterface.cpp` (it collapses each
  region to a single sub-band; we keep full EU sub-band granularity)
