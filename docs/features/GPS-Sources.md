# GPS sources

The badge's saved position (`gps_lat_e6`, `gps_lon_e6`) feeds two things: the
**Dist** column in the Nodes view (distance to each heard node) and the
**advert location** field when `advert_loc_policy` is set to *share*.

Four sources can write that NVS slot. The actively-used one is shown in
**Settings → Region & Location → GPS source**, so you can tell at a glance
which path produced the current coords. The values themselves all share one
NVS slot — whichever source writes last wins until the next push.

| # | Source | Setting / endpoint | Status |
|---|---|---|---|
| 1 | Manual entry | Settings → Region → GPS latitude / GPS longitude | ✅ Fully tested |
| 2 | PA1010D module on QWIIC | Settings → Region → "Auto-fill from GPS" action row | 🟡 Partial — one-shot scan works; periodic auto-refresh on an interval is **not** implemented yet |
| 3 | USB-CDC companion frame | Companion protocol `COMPANION_CMD_SET_ADVERT_LATLON` over USB CDC | 🟡 Preview — wire-format implemented, end-to-end roundtrip not verified |
| 4 | BLE companion | Same companion opcode but over NimBLE GATT | 🟡 Preview — iPhone MeshCore app can pair (passkey UI works) but lat/lon write path needs roundtrip verification |

## 1. Manual entry — ✅

Open **Settings → Region & Location**, press Enter on **GPS latitude**,
type the value, press Enter to commit. Repeat for **GPS longitude**.
Format: decimal degrees with negative for south / west, e.g.
`52.371234` for the Dam in Amsterdam.

- **Stored at**: NVS `system/gps_lat_e6` + `gps_lon_e6` (int32, scaled by 1e6
  per MeshCore upstream).
- **Source tag**: `GPS_SRC_MANUAL` — survives reboot.
- **Validation**: lat ∈ [-90, 90], lon ∈ [-180, 180]. Out-of-range entries
  are silently clamped at commit.

## 2. PA1010D on QWIIC — 🟡 Partial

Hardware: an Adafruit PA1010D mini-GPS breakout on the **QWIIC bus**
(GPIO33/SDA, GPIO32/SCL, I²C port 1, address 0x10). Works on both I²C
*and* UART, but the badge uses I²C — no wiring beyond a QWIIC cable.

**What's tested**: the one-shot "Auto-fill from GPS" scan reliably
captures a fix when the antenna has sky view, and the per-sat / HDOP
summary line works.

**What's not yet implemented**: a background task that refreshes the
NVS position every *N* minutes from the PA1010D without the user
pressing the action row. The current build only updates when the user
triggers it manually.

To capture a fix:

1. Plug the breakout into the QWIIC port. Top-side antenna up, clear sky-view.
2. Open **Settings → Region & Location → Auto-fill from GPS**, press Enter.
3. The toast shows `Searching GPS (30s)...`; the I²C reader blocks for up to
   30 seconds.
4. On a fix: toast shows `GPS fix: 52.37123, 4.89456 (8 sats)`, coords are
   saved to NVS with source tag `GPS_SRC_PA1010D`, and the row keeps showing
   `Last: 8 sats, HDOP 1.4` until you reboot or run another scan.
5. On no-fix the row reports `No fix - 4 sats visible (3G+1L)` so you can
   tell whether the antenna sees anything at all.

- **Drivers other than PA1010D**: the NMEA parser is talker-agnostic
  (`$GP/$GL/$GN/$BD/$GA`-prefixed `RMC`/`GGA`/`GSV`), so most MTK / Quectel
  modules at I²C address 0x10 work as-is. u-blox modules use address 0x42
  (DDC mode) — change `GPS_I2C_ADDR` in `components/mc_io/gps.c` if you swap chips. u-blox
  modules in UBX-binary mode aren't supported; switch them to NMEA via
  UBX-CFG-PRT first.

## 3. USB-CDC companion frame — 🟡 Preview

The companion-radio protocol used by the official MeshCore desktop +
mobile clients carries a `SET_ADVERT_LATLON` opcode (14) that pushes
`int32` lat/lon (×1e6) over USB CDC. The badge implements the parser
and saves with source tag `GPS_SRC_CDC`.

- **Wire format**: `<` + uint16 length + opcode 14 + 8 bytes
  (int32 lat_e6, int32 lon_e6, little-endian).
- **What works**: frame decode + NVS commit.
- **Not verified end-to-end**: which official client sends this opcode
  and over which transport-routing — desktop MeshCore push has not been
  smoke-tested here. Treat as preview until that roundtrip is run.

## 4. BLE companion — 🟡 Preview

Same opcode as USB-CDC, but over the NimBLE GATT companion service
(upstream Nicolai Electronics branch). Pairing works: the badge can
show the 6-digit passkey via a toast and accept the pair confirmation
from the iPhone MeshCore app. Position pushes from the app go through
the same companion-protocol parser, so when it works it lands in NVS
with source tag `GPS_SRC_BLE`.

- **What works**: NimBLE service registration, SMP passkey display,
  pair-done callback wiring.
- **Not verified end-to-end**: the iPhone MeshCore app's actual
  `SET_ADVERT_LATLON` write from a paired session has not been confirmed
  to land in NVS. Likely needs the upstream firmware-side BLE branch
  from Nicolai to be in sync. Treat as preview.

## Where the position is consumed

- **Nodes view → Dist column**: when `gps_position_valid` is true, every
  heard node with its own advert location gets a distance in km / m
  using the Haversine formula. Hidden when our own coords are unset.
- **Outgoing adverts**: when `advert_loc_policy == ADVERT_LOC_SHARE`,
  serialise our `gps_lat_e6` / `gps_lon_e6` into the ADVERT payload's
  location field. The toggle lives in the iPhone MeshCore app's Settings
  panel and is mirrored to NVS via `COMPANION_CMD_SET_OTHER_PARAMS`.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `GPS not detected on QWIIC` toast | Cable not seated / module shorted / wrong I²C address. Check with `i2c scan` on the firmware console. |
| `GPS silent (chip reachable but no NMEA)` | Module is in UBX-binary mode (u-blox) or hasn't completed cold boot. Wait 30 s and retry. |
| `No fix — N sats visible` for several minutes | Indoors / poor sky view. Move the antenna near a window or outside. |
