# Notification sounds

Four notification events — DM RX, channel RX, error, boot — each play
a WAV file from the SD card. The app does **not** ship audio samples;
you bring your own so we don't have to track licence terms for
redistributed clips. Pick from any free sound library, drop the files
on SD, choose one per event in **Settings → Sounds**.

## Layout on SD

Drop any number of `.wav` files (up to `SOUNDS_MAX_SLOTS = 20`) under
`/sd/meshcore/sounds/`. The filename — without the `.wav` extension —
is what the Settings picker shows for each event row. Names are
sorted alphabetically, so naming roughly determines the picker order.

```
/sd/meshcore/sounds/
  chime.wav
  ding.wav
  error.wav
  startup.wav
  …
```

Recommended: pick descriptive names so the picker is self-explanatory.
Anything past the 20th `.wav` (alphabetical order) is ignored.

## Settings → Sounds

Each event row cycles through:

```
Off → <first .wav> → <second .wav> → … → <SOUNDS_MAX_SLOTS-th .wav> → Off
```

| Settings row | NVS key | Default | Meaning |
|---|---|---|---|
| Volume | `snd.vol` | 50 | Master 0–100 % |
| DM sound | `snd.dm` | first WAV | Plays on direct-message RX |
| Channel sound | `snd.ch` | second WAV | Plays on channel RX |
| Error sound | `snd.err` | third WAV | Plays on internal errors (currently informational) |
| Boot sound | `snd.boot` | Off | Plays once on boot when set |

The internal "slot" is a `uint8_t` index (0 = Off, 1..N indexes into
the alphabetically sorted WAV list). It is **not** a filename; if you
rename a file later, the row will follow whatever sits at the same
alphabetical position. Replace the file in-place to keep the
assignment stable.

### Missing files

If the WAV that a slot points to is deleted from the SD, the row
shows `(missing #N)` instead of a filename — useful for spotting
broken pointers without having to remember the original assignment.

## WAV format

The player is a tiny RIFF/WAVE parser, not a full codec. Stick to:

| Property | Required value |
|---|---|
| Codec | PCM 16-bit signed |
| Sample rate | 22050 Hz (preferred — also 16000 / 32000 / 44100 / 48000 work) |
| Channels | 1 (mono) or 2 (stereo) |
| Tail | ~80 ms of silence so the I²S DMA flushes cleanly (added automatically by the player) |

> WAVs at other bit depths or compressed payloads (μ-law, ADPCM,
> MP3-in-WAV wrapper, …) are rejected with a `sounds: unsupported
> format` log line. Re-encode them first; see `ffmpeg` recipe below.

Keep each file **under ~200 KB** — that's roughly a 2-second mono clip
at 22050 Hz. Longer samples work but block the playback path for the
duration.

The basename can be up to `SOUNDS_NAME_MAX = 24` characters (excluding
`.wav`). Anything longer is truncated in the picker display.

## Recommended sources

Free, royalty-friendly catalogues. **You** are responsible for
respecting each clip's licence (most of these are CC0 or Pixabay's
own free-content licence — check at download time).

- [Pixabay → message sounds](https://pixabay.com/sound-effects/search/message/)
- [Pixabay → notification beeps](https://pixabay.com/sound-effects/search/notification/)
- [Pixabay → error / fail tones](https://pixabay.com/sound-effects/search/error/)
- [Pixabay → startup chimes](https://pixabay.com/sound-effects/search/startup/)
- [Freesound.org](https://freesound.org/) — broader catalogue, mind the per-clip licence

For each event, pick something short, distinct from the others, and
not so loud that it startles you in a meeting.

## Converting + uploading

Most downloads come as MP3 or 44 kHz stereo WAV. Re-encode to the
player's preferred format with `ffmpeg`:

```sh
ffmpeg -y -i input.mp3 -ar 22050 -ac 1 -c:a pcm_s16le chime.wav
```

| Flag | Meaning |
|---|---|
| `-ar 22050` | sample rate 22050 Hz |
| `-ac 1` | mono (use `2` if you want stereo) |
| `-c:a pcm_s16le` | PCM 16-bit signed little-endian |

Then upload via badgelink in USB-mode (no `--port`, libusb auto-discovers):

```sh
BL=path/to/badgelink.sh

# Make sure the destination directory exists.
$BL fs mkdir /sd/meshcore/sounds

# badgelink fs upload takes: badge_path  host_path
$BL fs upload /sd/meshcore/sounds/chime.wav  ./chime.wav
$BL fs upload /sd/meshcore/sounds/error.wav  ./error.wav
$BL fs upload /sd/meshcore/sounds/startup.wav ./startup.wav
```

> Argument order quirk: `badgelink fs upload` is **badge file first,
> host file second** — the opposite of `scp`. Easy to swap and get a
> `No such file or directory` error.

After uploading new files, reboot the badge (or pop into Settings →
Sounds and back) so `sounds_refresh()` rescans the directory.

## Testing

The bottom four rows in Settings → Sounds are action-row previews:

- **Preview DM** — plays whichever slot DM points to
- **Preview channel** — same for channel
- **Preview error** — same for error
- **Preview boot** — same for boot

Press Enter on the row to play. If you don't hear anything, in order:

1. Check the **Volume** row — `0%` mutes everything.
2. Check the event row — `Off` mutes only that event.
3. Run `Preview <event>` — bypasses the on/off flag, useful for "did
   the file even load" debugging.
4. Check the serial log for `sounds: ...` errors. Common ones:
   - `sounds: file not found /sd/meshcore/sounds/<name>.wav` — slot
     pointer is stale (the row should show `(missing #N)` if so).
   - `sounds: unsupported format` — re-encode with the ffmpeg recipe
     above.
   - `sounds: bsp_audio not ready` — amplifier failed to power on;
     try a reboot.

## Why no bundled samples?

Licensing fragility. Pixabay's content licence is permissive but their
TOS allows individual creators to upload work with extra restrictions
(no AI training, attribution required, etc.), enforced per-clip. Same
goes for Freesound — most CC0, some CC-BY, a few CC-BY-NC. Bundling a
clip in this repo under our MIT licence would mean tracking each
upstream licence in lockstep with any clip rotation. Asking you to
pick your own keeps that responsibility where the law puts it.

## Related
- [Settings / NVS](Settings-NVS.md) — `snd.vol` / `snd.dm` / `snd.ch` / `snd.err` / `snd.boot` keys
- [SD card layout](SD-Card-Layout.md) — `/sd/meshcore/` directory structure
