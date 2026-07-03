# Keyboard input & the character picker

How text — including umlauts, accents and symbols — gets into a message on the
Tanmatsu, and how the app draws it back. Two independent routes exist: **typing**
(including the AltGr layer) and the **F5 character picker** overlay.

## The keyboard event

The badge BSP delivers each key as a `bsp_input_event_args_keyboard_t` with three
fields ([badge_bsp_input.c](https://github.com/badgeteam/esp32-component-badge-bsp/blob/main/targets/tanmatsu/badge_bsp_input.c)):

| Field | Meaning |
|---|---|
| `char ascii` | the plain ASCII byte (`'q'`, `'\r'`, `0x7F` …), or `0` if the key has none |
| `char const* utf8` | the UTF-8 string for the key **including the active modifier layer** — this is where AltGr characters arrive |
| `uint32_t modifiers` | SHIFT / CTRL / ALT_L / **ALT_R (AltGr)** / SUPER bitmask |

The driver resolves the layer *before* sending: with AltGr (`ALT_R`) held it puts
the key's AltGr variant into `utf8`, so the app just reads `utf8` — it does not
track modifiers itself. `main.c` forwards both to `handle_key(ascii, utf8)`.

## Function keys

The coloured keys on the top row map to `BSP_INPUT_NAVIGATION_KEY_F1..F6`:

| Key | Symbol | In a chat |
|---|---|---|
| F1 | red ✕ | cancel typing / back |
| F4 | green ◯ | open the **emoji** picker |
| F5 | blue ☁ (cloud) | open the **special-character** picker |
| ESC | — | exits only from the home tile grid |

## Typing special characters — the AltGr layer

Hold **AltGr** and press a key to type an accented / special character directly,
no picker needed. The map below is the Tanmatsu default layout (see
[badge_bsp_input.c L352+](https://github.com/badgeteam/esp32-component-badge-bsp/blob/main/targets/tanmatsu/badge_bsp_input.c#L352)).
`handle_key` inserts the AltGr `utf8` when it decodes to a character the display
font can draw; otherwise it falls back to the base key.

Letters (AltGr / AltGr+Shift):

| Key | AltGr | +Shift | | Key | AltGr | +Shift |
|---|---|---|---|---|---|---|
| Q | ä | Ä | | A | á | Á |
| W | å | Å | | S | ß | § |
| E | é | É | | D | ð | Ð |
| R | ® | ™\* | | F | ë | Ë |
| T | þ | Þ | | J | ï | Ï |
| Y | ü | Ü | | K | œ\* | Œ\* |
| U | ú | Ú | | L | ø | — |
| I | í | Í | | Z | æ | Æ |
| O | ó | Ó | | X | · | — |
| P | ö | Ö | | C | © | ¢ |
| | | | | N | ñ | Ñ |
| | | | | M | µ | ± |

Number / symbol row (AltGr): `1`→¡ `2`→² `3`→³ `4`→¤ `5`→**€** `6`→¼ `7`→½
`8`→¾ `-`→¥ `[`→« `]`→» `/`→¿ `\`→¬

\* Characters outside the extended display font — **œ/Œ, ™, the smart quotes
`9/0/[/]`+Shift give, and the combining diacritics** on the number row and
`;',.` — are **not** drawn by the font, so the app inserts the base key instead
of a glyph it cannot render. Everything else in the table renders.

## The character picker (F5)

For discoverability, or when a character isn't on the AltGr layer, press **F5**
(blue cloud) while typing in a chat/channel. It opens a paged 4×10 overlay of the
40 most-used European characters, ordered by frequency:

```
ä ö ü ß   Ä Ö Ü °   € § µ ñ   é è ê ë   à â á ç
î ï í ô   û ù ú ó   å Å æ Æ   ø Ø ¿ ¡   « » – —
```

Navigation mirrors the F4 emoji picker exactly:

| Input | Action |
|---|---|
| D-pad / **W A S D** | move the cursor (W/S also page) |
| **Enter** / D-pad centre | insert the selected character |
| **F1** (red ✕) or **F5** again | close the overlay |

One overlay serves both banks (`emoji_picker_mode` = `PICKER_EMOJI` /
`PICKER_SPECIAL`); the UI drives it through `picker_count()` / `picker_entry()`.
The character bank lives in `components/mc_common/special_table.c`.

## How the app draws it back

Two things have to allow a non-ASCII codepoint or it becomes `?`:

1. **The display font.** Chat uses extended Montserrat faces
   (`components/mc_fonts/`) covering ASCII + Latin-1 Supplement + `€` + en/em
   dash, generated with `scripts/gen_ext_fonts.sh` (**`--no-compress`** is
   required — see [Special-Characters.md](Special-Characters.md)).
2. **Two `?`-filters.** `utf8_sanitize` (in `chat.c`, on receive) and
   `emoji_text` (in `lvgl_ui.c`, on draw) both used to collapse any non-emoji
   multi-byte sequence to `?`. Both now consult `special_font_covers()` and pass
   through anything the font can render — otherwise a genuinely undrawable
   codepoint still degrades to `?`.

`special_font_covers()` is the single source of truth for "can we draw this
codepoint", kept in lockstep with the font ranges in `gen_ext_fonts.sh`.

## See also

- [Special-Characters.md](Special-Characters.md) — the display font + the 40-char
  bank rationale and how to regenerate the fonts.
- Upstream keyboard driver + full keymap:
  [badgeteam/esp32-component-badge-bsp](https://github.com/badgeteam/esp32-component-badge-bsp/blob/main/targets/tanmatsu/badge_bsp_input.c).
- Hardware / firmware docs:
  [Nicolai-Electronics/tanmatsu-documentation](https://github.com/Nicolai-Electronics/tanmatsu-documentation).
