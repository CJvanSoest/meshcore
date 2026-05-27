# UI / UX

The UI is a single full-screen framebuffer (PAX-GFX) rendered after every
input event and at ~1 Hz idle ticks for live counters.

## Layout constants

Defined in `main/render.h`:

| Constant | Value | Purpose |
|---|---|---|
| `TAB_BAR_H` | 44 | Top tab strip height |
| `FOOTER_H` | 28 | Single-line footer height (chat/channel) |
| `CHAT_ROW_H` | 44 | Per-message row height |
| `CHAT_Y0` | `TAB_BAR_H + 4` | First chat row Y |
| `CHAT_INPUT_H` | 36 | Input bar height (above footer) |

Settings and Nodes tabs use a local `footer_h = 60` for their 2-line footer
(controls + timestamp / advert age).

## Tokyo Night palette

| Token | Hex | Use |
|---|---|---|
| `COL_BG` | `#1A1B26` | Main background |
| `COL_HEADER` | `#16161E` | Tab bar, footer surface |
| `COL_PANEL` | `#24283B` | Row highlight, separators, message bubbles |
| `COL_WHITE` | `#C0CAF5` | Body text |
| `COL_GRAY` | `#565F89` | Dim / secondary text |
| `COL_ACCENT` | `#7AA2F7` | Selection, mine-bubble text, REPEATER role |
| `COL_GREEN` | `#9ECE6A` | OK, online, signal good |
| `COL_AMBER` | `#E0AF68` | Heading, edit-mode, signal medium |
| `COL_RED` | `#F7768E` | Error, signal poor |
| (Purple) | `#BB9AF7` | ROOM_SERVER role |

## Tabs

| Tab | Entry behaviour |
|---|---|
| Settings | Field list with cursor; Enter enters edit mode |
| Nodes | Live heard-node list with column header (Role / Name / RSSI / SNR / #Pkt / Dist / Seen) |
| DM | If no target: inbox view (saved contacts + active DM target). If target set: per-peer conversation |
| Channel | Public channel chat |

`Tab` rotates between the four. `current_view` is `app_view_t` defined in
`app_config.h`.

## Edit-mode state machine (Settings tab)

```
   ─── view ───►  ENTER  ───► editing ───► ESC ───►  view
                                  │
                                  │  Backspace / Up / Down
                                  ▼
                              edit value
                                  │
                                  │  ENTER (save) → NVS write → view
                                  ▼
                              text-edit buffer
                              (FIELD_OWNER / FIELD_ADV_NAME /
                               FIELD_REGION_SCOPE)
```

`edit_mode` and `field_editing_text` are the two flags. `field_edit_buf` is
a shared 33-byte text scratch; `selected` is the current row index.

## DM inbox mode (`dm_inbox_mode`)

The DM tab has two states:

1. **Inbox** — list of conversations (active DM target on top, then saved
   contacts). Avatar shows first letter of the name in an amber square for
   the active target, blue for saved contacts.
2. **Conversation** — picked by pressing Enter on an inbox row, or by
   pressing Enter on a node in the Nodes tab.

Press ESC inside a conversation to return to inbox.

## QR overlay (`qr_overlay_active`)

Triggered by `Q` in the Nodes tab. Renders a full-screen QR code encoding
`meshcore://contact/add?name=<adv>&public_key=<hex>&type=1`. Any keypress
closes it. The encoding uses `qrcodegen` with ECC_MEDIUM and version 1..10.

The QR buffers are `static uint8_t qr_data[qrcodegen_BUFFER_LEN_MAX]` etc.
to avoid stack overflow (each buffer is ~3.9 KB).

## Radio bootloader screen (`radio_bootloader_mode`)

Triggered by `U` in Settings. The screen replaces the tab bar with a blue
"Radio Bootloader Mode" header and displays the esptool command for the C6
flash. The escape route is `ESC` / `F1` which power-cycles the badge.

## Battery & RX indicator

Always painted at the top-right of the tab bar by `render_tab_bar`:

- `RX:<count>` — packets seen since boot (green)
- `<pct>%[+]` — battery percentage; `+` suffix means charging; coloured
  green / amber / red by level

## Notification LED

`chat.c` toggles the bottom-left LED:

| Colour | Condition |
|---|---|
| Green | At least one unseen DM |
| Blue | At least one unseen channel message |
| Off | The relevant tab is open or no pending message |

`update_notification_led` is called from chat add helpers and tab switch.

## Channel list mode (`channel_list_mode`)

The Channel tab opens in list mode by default — a scrollable list of
joined channels (Public is always slot 0, hardcoded `PUBLIC_GROUP_PSK`).

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
MeshCore-Protocol wiki page).

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
const arrays in `main/emoji_bitmaps.c` (CC-BY 4.0 Twemoji). The chat
input bar renders through `emoji_draw_text` so staged emoji are visible
inline before Enter sends the message.
