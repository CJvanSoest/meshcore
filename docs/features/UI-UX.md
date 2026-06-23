# UI / UX

The UI is a single full-screen framebuffer (PAX-GFX) rendered after every
input event and at ~1 Hz idle ticks for live counters. As of v2.2.0 the
classic tab-bar is replaced with a shared Pager-style status strip on every
view, and boot lands on a tile-grid home screen instead of Settings.

## Layout constants

Defined in `components/mc_ui/render.h`:

| Constant | Value | Purpose |
|---|---|---|
| `TAB_BAR_H` | 44 | Top status-strip height (the "Pager header") |
| `FOOTER_H` | 28 | Single-line footer height (chat/channel) |
| `CHAT_ROW_H` | 44 | Per-message row height |
| `CHAT_Y0` | `TAB_BAR_H + 4` | First chat row Y |
| `CHAT_INPUT_H` | 36 | Input bar height (above footer) |

Home tile-grid geometry lives in `render_home.c` as `HOME_HEADER_H=50`,
`HOME_FOOTER_H=38`, `HOME_H_MARGIN=30`, `HOME_V_MARGIN=20`, with a 4×2 tile
layout. Settings tile-grid geometry mirrors the same numbers in
`render_settings.c`.

## Palettes

The classic views (Settings drilldown, Nodes, DM, Channel) use the Tokyo
Night palette; home and About use the LilyGo Pager palette. The Pager
status strip is rendered in Pager colours and overlays every view.

### Tokyo Night

| Token | Hex | Use |
|---|---|---|
| `COL_BG` | `#1A1B26` | Main background |
| `COL_HEADER` | `#16161E` | Footer surface |
| `COL_PANEL` | `#24283B` | Row highlight, separators, message bubbles |
| `COL_WHITE` | `#C0CAF5` | Body text |
| `COL_GRAY` | `#565F89` | Dim / secondary text |
| `COL_ACCENT` | `#7AA2F7` | Selection, mine-bubble text, REPEATER role |
| `COL_GREEN` | `#9ECE6A` | OK, online, signal good |
| `COL_AMBER` | `#E0AF68` | Heading, edit-mode, signal medium |
| `COL_RED` | `#F7768E` | Error, signal poor |
| (Purple) | `#BB9AF7` | ROOM_SERVER role |

### LilyGo Pager (home, About, status strip)

| Token | Hex | Use |
|---|---|---|
| `COL_PAGER_BG` | `#0E141B` | Window background |
| `COL_PAGER_TILE` | `#16161E` | Unfocused tile surface |
| `COL_PAGER_TEXT` | `#C0C8D0` | Body / label text |
| `COL_PAGER_ACCENT` | `#FAA61A` | Focused tile, highlights (orange) |

## Views

| View | Enum | Entry behaviour |
|---|---|---|
| Home | `VIEW_HOME` | 4×2 tile-grid (Nodes / DM / Channel / Map / Advert / Settings / About / QR); WSAD or D-pad to focus, Enter opens |
| Settings | `VIEW_SETTINGS` | Tile-grid of 7 drilldown categories + 1 external Toolbox tile (+ a hidden Advert category) → Enter drills in to a category's field list, or opens the Toolbox launcher |
| Nodes | `VIEW_NODES` | Live heard-node list with column header (Role / Name / RSSI / SNR / #Pkt / Dist / Seen) |
| DM | `VIEW_CHAT` | If no target: inbox view (saved contacts + active DM target). If target set: per-peer conversation |
| Channel | `VIEW_CHANNEL` | Public channel chat |
| About | `VIEW_ABOUT` | Version + build date + author + upstream credits + license + source URL |
| Toolbox | `VIEW_TOOLBOX` | Launcher for LoRa diagnostic tools, reached from the Settings Toolbox tile (see the Toolbox section) |

`Tab` rotates through the four classic views (`VIEW_TAB_COUNT=4`) — home,
About and the Toolbox views sit outside the tab carousel. `current_view` is
`app_view_t` defined in `app_config.h`; boot default is `VIEW_HOME`.

ESC falls through a back-stack: drilldown → category list → home →
launcher. This makes home the safe "I am here" anchor across the app. The
Toolbox views add their own leg: packet-log / coverage → Toolbox launcher →
Settings.

## Home tile grid (`render_home.c`)

4 columns × 2 rows = 8 tiles. Each tile carries:

- A PAX-drawn icon (lines / circles / triangles — no bundled bitmaps yet)
- A centered label (supports embedded `\n` for multi-line names)
- An optional `home_badge_fn` callback (DM / Channel return their unread
  totals via `contact_unread_total` / `channel_unread_total`, drawn as a
  red pill in the top-right corner of the tile)
- An optional `home_action_t` (Advert tile = `HOME_ACTION_SEND_ADVERT`
  fires `send_advert()` inline + 2-second toast; QR tile =
  `HOME_ACTION_OPEN_QR` flips on the QR overlay and tracks origin so ESC
  returns to home)

A tile is rendered as a "soon" placeholder (dim foreground + small
`soon` subtitle) only when its target is `VIEW_HOME` *and* it carries
no action — so Advert reads as live even though it doesn't switch view.

## Pager status strip (`render_tab_bar`)

Shared header on every classic view. Left side: view name (e.g. "Nodes")
plus inline DM / # unread badges if the *other* tabs have unreads. Right
side, in this order: RX count, TX (rolling 1 h duty cycle %), battery
percentage with charging suffix. Battery turns amber/red by level; TX
turns amber at ≥80 % budget and red when the last TX was blocked by
duty-cycle enforcement.

The home screen draws its own taller (50 px) header with the advert/owner
name on the left instead of a view name; same right-side stats.

## Settings drilldown (`render_settings.c`)

Two levels:

1. **Category list** (`settings_category_list_mode == true`) — 4-column
   Pager tile grid, seven drilldown tiles: Identity / Regulatory / Radio /
   Network / Region & Location / Brightness / Sounds, plus an external
   **Toolbox** tile (marked by `first == FIELD_COUNT`) that opens the Toolbox
   launcher instead of a field list. (A further category, Advert, is hidden
   from this grid and reached via the Home -> Advert tile.) Each tile has
   its own PAX-drawn category icon. Multi-line labels via embedded `\n` so the
   wider "Region & Location" wraps onto two lines. The per-category field lists
   live in `s_categories[]` (`render_settings.c`): Identity = owner/advert name
   + radio firmware; Regulatory = country, antenna gain, duty cycle; Radio =
   freq, SF, BW, CR, power, sync, preamble, preset; Network = role, path hash,
   WiFi, HTTP endpoint, BLE; Region & Location = region scope, GPS coordinates,
   GPS source/profile, map style; Brightness = display, keyboard, RGB LED,
   auto-blank; Sounds = volume + per-event toggles + previews.
2. **Drilldown** (`settings_category_list_mode == false`) — Tokyo Night
   row list, but only the fields belonging to `settings_category_active`.
   The category title is the amber page header.

`settings_category_bounds(cat, &first, &last)` (declared in
`render_internal.h`) drives the input clamp so navigation wraps within
the active category instead of cycling all `FIELD_COUNT` fields. Tile
navigation from the category list is `±1` horizontal, `±4` vertical
(matching the 4-column layout) for both D-pad and WSAD.

| Category | Fields |
|---|---|
| Identity | Radio ID, Radio firmware, Owner name, Advert name |
| Regulatory | Country, Antenna gain, Duty cycle |
| Radio | Frequency, SF, BW, CR, TX power, Sync word, Preamble, LoRa preset, RX sensitivity |
| Network | Advert interval, Role, Path hash size |
| Region & Location | Region scope, GPS latitude, GPS longitude |
| Brightness | Display backlight, Keyboard backlight, RGB LED brightness |

Opening Settings from the home tile resets to the category list; opening
via `Tab` cycle preserves the last drilled-in category so power users can
flip between views without losing their place.

## Brightness category (#47)

Three per-app sliders that override the launcher backlight/LED globals
while MeshCore is running:

| Field | NVS key | Default | BSP call on change |
|---|---|---|---|
| Display backlight | `ui.disp_bl` | 50 | `bsp_display_set_backlight_brightness(pct)` |
| Keyboard backlight | `ui.kb_bl` | 50 | `bsp_input_set_backlight_brightness(pct)` |
| RGB LED brightness | `ui.led_br` | 5 | `bsp_led_set_brightness(pct)` |

All three cycle through the same 5/10/25/50/75/100 % stops as the other
sliders. `field_adjust` calls `apply_brightness()` live so the user sees
the change while scrolling; Enter persists to NVS via `save_brightness()`.
`load_brightness()` + `apply_brightness()` run at boot in `main.c` after
the rest of the NVS loaders so the per-app values are applied before the
first frame.

Restore-on-exit is intentionally **not** implemented — BadgeVMS PIE ELF
apps have no clean exit hook (see [BadgeVMS callback unsafe gotcha] in
memory). The launcher overrides the values again as soon as it cycles
back to its own settings.

## About view (`render_about.c`)

Version + build date are pulled live from `esp_app_get_description()` so
a clean tag (e.g. `v2.4.0`) produces a clean string with no `-dirty` /
post-tag suffix. The rest is static: author (CJ van Soest), credits
(MeshCore by Ripple Radios; Tanmatsu by Nicolai Electronics), MIT
license, source URL, issue tracker. Footer: `ESC: home`.

When more items get added (commit hash, region preset, map license credits
once maps land) prefer inline `Label: value` per line over the current
label-stacked-above-value layout, per CJ's feedback on the v2.2.0 design.

## Toolbox (#3)

A diagnostics launcher reached from the Settings **Toolbox** tile (an external
category, so it switches to `VIEW_TOOLBOX` instead of drilling into a field
list). The launcher lists sub-tools; `WS` / D-pad move the cursor, Enter opens,
ESC returns to Settings. A not-yet-built tool renders dimmed with a "soon" tag.

### Packet Log (`VIEW_TOOLBOX_LOG`)

A live, terminal-style log of every radio frame in both directions, fed by a
capture ring (`mc_common/diag`) tapped in `mc_radio` (the RX task and
`radio_tx_message`). Two modes toggle with `H`:

- **Hex** — the leading on-air bytes as a hex dump.
- **Dissector** — per-payload fields decoded by the pure `mc_proto/diag_decode`
  (type, route, hops, RSSI/SNR, ADVERT pubkey/role/name, DM dst/src hash).
  Public channel (GRP_TXT) senders are anonymous by protocol design.

`P` pauses (freezes the displayed window while capture keeps running), `WS` /
D-pad scroll, `C` clears, `E` exports the ring to a CSV on SD
(`/sd/meshcore/log/pkt_<unix>.csv`, see
[SD-Card-Layout.md](../reference/SD-Card-Layout.md); a toast reports the path +
count), ESC returns to the launcher. Export lives on `E` rather than `S`
because `S` is the scroll-down key here. Each captured frame is dissected once
at snapshot time, so the render loop never re-decodes a visible row. The dissect
runs on the captured prefix (`DIAG_RAW_MAX` = 176 B), so a longer frame shows a
display-only truncation; header fields stay complete.

### Coverage Test (`VIEW_TOOLBOX_COVERAGE`)

A repeater reachability tester. The view lists discovered repeaters
(`role == REPEATER`) with an `x/3` counter and a green/orange/red status. Enter
pings the selected repeater 3x at a 10 s interval — a background task in `mc_rx`
sends an upstream MeshCore **TRACE** (the only reachability probe a repeater
answers without an admin login) and matches the returning frame by tag in
`rx_handle_trace`, recording reachability plus uplink/downlink SNR — classifies
the result (3/3 green, 1-2 orange, 0 red), and appends every GPS-stamped attempt
to one CSV per session on SD (`/sd/meshcore/coverage/`, see
[SD-Card-Layout.md](../reference/SD-Card-Layout.md)). `WS` / D-pad move the
cursor, Enter pings, `R` starts a new session, ESC returns to the launcher.
Results and the SD log live in `mc_domain/coverage`; the TRACE payload layout is
the pure `mc_proto/trace`. The coverage map (z15/16 markers + PNG export) is a
later sub-phase; see [Toolbox.md](Toolbox.md).

## Edit-mode state machine (Settings drilldown)

```
   ─── drilldown ───►  ENTER  ───► editing ───► ESC ───►  drilldown
                                       │
                                       │  Backspace / Up / Down
                                       ▼
                                  edit value
                                       │
                                       │  ENTER (save) → NVS write → drilldown
                                       ▼
                                  text-edit buffer
                                  (FIELD_OWNER / FIELD_ADV_NAME /
                                   FIELD_REGION_SCOPE / FIELD_GPS_*)
```

`edit_mode` and `field_editing_text` are the two flags. `field_edit_buf`
is a shared 33-byte text scratch; `selected` is the current row index
(clamped to the active category's bounds).

## DM inbox mode (`dm_inbox_mode`)

The DM view has two states:

1. **Inbox** — list of conversations (active DM target on top, then
   saved contacts). Avatar shows first letter of the name in an amber
   square for the active target, blue for saved contacts. Per-contact
   unread badges appear right of the name.
2. **Conversation** — picked by pressing Enter on an inbox row, or by
   pressing Enter on a node in the Nodes view.

Press ESC inside a conversation to return to inbox; ESC on the inbox
falls through to home.

## QR overlay (`qr_overlay_active`)

Triggered either by the **QR home tile** (opens with `qr_from_home = true`
so closing returns to home) or by `Q` in the Nodes view (stays in Nodes
on close). Renders a full-screen QR encoding
`meshcore://contact/add?name=<adv>&public_key=<hex>&type=1` using
`qrcodegen` (ECC_MEDIUM, version 1..10).

Close hint at the bottom: `[ESC] [X] [Enter] to close` in amber, so the
keys + the Tanmatsu's physical red X button are all signposted.

The QR buffers are `static uint8_t qr_data[qrcodegen_BUFFER_LEN_MAX]` etc.
to avoid stack overflow (~3.9 KB each).

## Status toast

`render_home.c` paints a centered Pager-style toast box for ~2 seconds
when `toast_text[0] != '\0'`. Used by the Advert tile ("Flood advert
sent"); auto-clears once the timestamp ages out. Future action tiles can
re-use the same `toast_text` / `toast_start_ms` globals.

## Battery / RX / TX indicator

Painted in the right half of the Pager strip by `render_tab_bar`:

- `RX:<count>` — packets seen since boot (green)
- `TX:<pct>%` — rolling 1-hour duty-cycle usage. Pager-text colour
  normally, amber at ≥80 %, red when the last TX was blocked.
- `<pct>%[+]` — battery percentage; `+` suffix means charging; coloured
  green / amber / red by level

The home screen footer also mirrors the Settings tab's bottom-row stats
on the right (`RX:<rssi> SNR:<snr> N:<noise>`) so the landing screen
doubles as a quick radio dashboard.

## Notification LED

`chat.c` toggles the bottom-left LED:

| Colour | Condition |
|---|---|
| Green | At least one unseen DM |
| Blue | At least one unseen channel message |
| Off | The relevant view is open or no pending message |

`update_notification_led` is called from chat add helpers and on view
switches.

## Channel list mode (`channel_list_mode`)

Channel view opens in list mode by default — a scrollable list of joined
channels (Public is always slot 0, hardcoded `PUBLIC_GROUP_PSK`).

| Key | Action |
|---|---|
| `W` / `S` / D-pad UP/DOWN | Cursor up / down |
| Enter / RETURN | Select the channel (switches `active_channel_idx`) + flip to chat view |
| `A` | Begin add-channel text input (auto-prefix `#`) |
| `D` | Delete the cursor's channel (Public protected) |
| `ESC` from chat | Back to list |

The chat-view header shows the active channel name on the first line and
`Region: <scope>` on the second (amber `(set in Settings)` placeholder if
`region_scope` NVS is empty). On the wire region scope is encoded as
`transport_codes` on `ROUTE_TYPE_TRANSPORT_FLOOD` packets (see the
MeshCore-Protocol page).

## Emoji picker (`emoji_picker_active`)

Triggered with the **green ◯ button (F4)** while `chat_typing == true`
in either DM or Channel chat. Renders a 2×4 grid of Twemoji bitmaps on
top of the chat input.

| Key | Action |
|---|---|
| D-pad LEFT/RIGHT/UP/DOWN, `A`/`D`/`W`/`S` | Cursor across the grid |
| Enter / RETURN | Insert the UTF-8 bytes into `chat_input`; close picker |
| `ESC` / `F1` | Close without inserting |

The MVP set is 8 codepoints in U+1F60x (`grin`, `smile`, `wink`, `blush`,
`cool`, `tongue`, `cry`, `angry`). Bitmaps are 32×32 ARGB embedded as
const arrays in `components/vendor/emoji_bitmaps.c` (CC-BY 4.0 Twemoji). The chat
input bar renders through `emoji_draw_text` so staged emoji are visible
inline before Enter sends the message.
