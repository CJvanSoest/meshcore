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
