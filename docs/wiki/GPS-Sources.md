# GPS sources

The badge's saved position (`gps_lat_e6`, `gps_lon_e6`) feeds two things: the
**Dist** column in the Nodes view (distance to each heard node) and the
**advert location** field when `advert_loc_policy` is set to *share*.

Five sources can write that NVS slot. The actively-used one is shown in
**Settings → Region & Location → GPS source**, so you can tell at a glance
which path produced the current coords. The values themselves all share one
NVS slot — whichever source writes last wins until the next push.

| # | Source | Setting / endpoint | Status |
|---|---|---|---|
| 1 | Manual entry | Settings → Region → GPS latitude / GPS longitude | ✅ Fully tested |
| 2 | PA1010D module on QWIIC | Settings → Region → "Auto-fill from GPS" action row | 🟡 Partial — one-shot scan works; periodic auto-refresh on an interval is **not** implemented yet |
| 3 | HTTPS `/ping` endpoint | `POST https://tanmatsu.local:8443/ping` | ✅ Fully tested with **OwnTracks** ; 🟡 MeshMapper + iOS Shortcuts payload shapes are implemented but not yet roundtrip-verified |
| 4 | USB-CDC companion frame | Companion protocol `COMPANION_CMD_SET_ADVERT_LATLON` over USB CDC | 🟡 Preview — wire-format implemented, end-to-end roundtrip not verified |
| 5 | BLE companion | Same companion opcode but over NimBLE GATT | 🟡 Preview — iPhone MeshCore app can pair (passkey UI works) but lat/lon write path needs roundtrip verification |

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
  (DDC mode) — change `GPS_I2C_ADDR` in `main/gps.c` if you swap chips. u-blox
  modules in UBX-binary mode aren't supported; switch them to NMEA via
  UBX-CFG-PRT first.

## 3. HTTPS `/ping` endpoint — ✅ OwnTracks fully tested, others 🟡 preview

The badge runs an HTTPS server on port 8443 whenever WiFi is up.
`POST /ping` accepts a JSON body with lat/lon and writes them to the NVS
slot with source tag `GPS_SRC_HTTP`. The endpoint is **authenticated** —
you must pass the badge's API key either as the `X-API-Key` header or as
the `?key=…` query parameter (iOS Shortcuts strips custom headers when
the body type is JSON, hence the query-param fallback).

Three payload shapes are accepted by the parser, but only the OwnTracks
flow has been verified end-to-end so far:

| Payload shape | Status |
|---|---|
| **OwnTracks `_type=location`** | ✅ Fully tested — coords land in NVS, badge advert position updates |
| MeshMapper batch (`{"data":[…]}`) | 🟡 Preview — parser implemented, **not** yet roundtrip-verified against a real MeshMapper sender |
| iOS Shortcut flat push (`{"lat":"…","lon":"…","timestamp":…}`) | 🟡 Preview — parser handles the Text-typed lat/lon quirk, **not** yet roundtrip-verified from a built Shortcut |

All three payload shapes are documented below for completeness; treat
the preview ones as "this is the format — please report back if it
works for you" until they're verified.

- **MeshMapper batch**: `{"data":[{"lat":…,"lon":…,"timestamp":…}, …]}` — up
  to 50 entries per batch, the highest-timestamp valid one wins.
- **Flat single push** (iOS Shortcuts):
  `{"lat":"52.371234","lon":"4.894560","timestamp":1717920000}`. Note
  *strings* not numbers — Shortcuts drops the decimal point from Number
  fields, so mark lat/lon as Text in the Shortcut UI.
- **OwnTracks HTTP**: `{"_type":"location","lat":…,"lon":…,"tst":…,…}` —
  other `_type` values (`transition`, `waypoint`, `cmd`) are acknowledged
  with an empty `[]` so the OwnTracks app stops retrying, without touching
  the badge's coords.

### Setting up an iOS Shortcut

1. **Connect to the same WiFi as the badge.** mDNS resolution
   (`tanmatsu.local`) requires both devices on the same broadcast domain.
2. **Trust the cert.** On the iPhone, visit
   `https://tanmatsu.local:8443/cert` in Safari, install the downloaded
   profile (Settings → General → VPN & Device Management → Profile), then
   enable it under Settings → General → About → Certificate Trust Settings.
   The cert is per-badge and stable across reboots, so this is a one-time
   step.
3. **Find the API key.** Settings → Network → **API key** shows the first
   8 + last 4 hex chars; the full 64-char key lives in NVS and is printed
   on serial at boot. You can also rotate it from **Regenerate API key**.
4. **Build the Shortcut**:
   - Action: *Get Current Location*
   - Action: *Get Contents of URL* —
     URL: `https://tanmatsu.local:8443/ping?key=<YOUR_KEY>`,
     Method: `POST`, Request Body: `JSON` with fields
     `lat` (Text → Location.Latitude), `lon` (Text → Location.Longitude),
     `timestamp` (Number → Current Date → Unix Time).

### Setting up OwnTracks

OwnTracks supports HTTP mode out of the box:

- **Mode**: HTTP
- **Host**: `tanmatsu.local`
- **Port**: `8443`
- **TLS**: on (Use Identity if you've installed the badge cert; otherwise
  accept the self-signed cert prompt)
- **Identification → URL**: `/ping?key=<YOUR_KEY>`

Once the badge cert is installed as a trusted profile, OwnTracks won't
prompt again. Coords push every ~30 minutes on cellular, near-instant on
significant location changes.

### Setting up MeshMapper

MeshMapper's wardrive sender accepts any URL:

- **Server**: `https://tanmatsu.local:8443/ping?key=<YOUR_KEY>`
- It batches up to 50 entries every ~15–30 s; the badge keeps only the
  newest valid one as its position.

## 4. USB-CDC companion frame — 🟡 Preview

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

## 5. BLE companion — 🟡 Preview

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
| OwnTracks fails TLS handshake | Cert profile not trusted on iPhone. Re-visit `https://tanmatsu.local:8443/cert` and toggle the profile under Certificate Trust Settings. |
| `Specified method is invalid for this resource` in browser | You hit `/ping` with a GET (browsers always GET). Open the root `/` instead — it lists the endpoints. |
| iOS Shortcut `lat` arrives as `5187...` instead of `5.1871...` | Field marked as *Number* — change to *Text* in the Shortcut UI. |
| `mDNS resolution failure` | iPhone and badge not on the same WiFi (or guest-isolated). Try the raw IP first: the **HTTPS endpoint** row in Settings shows it. |
