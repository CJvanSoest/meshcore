# Notification sounds

Four event slots — DM RX, channel RX, error, boot — each play one WAV
file from the SD card. The app does **not** ship audio samples; you
bring your own so we don't have to track licence terms for redistributed
clips. Pick from any free sound library, drop the files on SD, set the
slot in **Settings → Sounds**, done.

## Layout on SD

```
/sd/meshcore/sounds/
  1.wav     # default DM slot
  2.wav     # default channel slot
  3.wav     # default error slot (played on error paths + the Sounds preview)
  4.wav     # default boot slot
```

The Settings → Sounds picker maps each event to a slot:

| Settings row | NVS key | Default slot |
|---|---|---|
| DM sound | `system/snd_dm` | 1 |
| Channel sound | `system/snd_ch` | 2 |
| Error sound | `system/snd_err` | 3 |
| Boot sound | `system/snd_boot` | 4 |

Volume is shared (`system/snd_vol`, 0–100 %). Set a slot to `0` (off)
to disable that event.

## WAV format

The player is a tiny RIFF/WAVE parser, not a full codec. Stick to:

| Property | Required value |
|---|---|
| Codec | PCM 16-bit signed |
| Sample rate | 22050 Hz (preferred — also 16000 / 32000 / 44100 / 48000 work) |
| Channels | 1 (mono) or 2 (stereo) |
| Tail | ~80 ms of silence so the I2S DMA flushes cleanly (added automatically by the player) |

> WAVs at other bit depths or compressed payloads (μ-law, ADPCM, MP3-in-WAV
> wrapper, …) will be rejected with a `sounds: unsupported format` log
> line. Re-encode them first; see `ffmpeg` recipe below.

Keep each file **under ~200 KB** — that's roughly a 2-second mono clip
at 22050 Hz. Longer samples work but block the UI for the duration.

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
ffmpeg -y -i input.mp3 -ar 22050 -ac 1 -c:a pcm_s16le 1.wav
```

| Flag | Meaning |
|---|---|
| `-ar 22050` | sample rate 22050 Hz |
| `-ac 1` | mono (use `2` if you want stereo) |
| `-c:a pcm_s16le` | PCM 16-bit signed little-endian |

Repeat for `2.wav`, `3.wav`, `4.wav`. Then upload via badgelink in
USB-mode (no `--port`, libusb auto-discovers):

```sh
BL=path/to/badgelink.sh

# Make sure the destination directory exists.
$BL fs mkdir /sd/meshcore/sounds

# badgelink fs upload takes: badge_path  host_path
$BL fs upload /sd/meshcore/sounds/1.wav  ./1.wav
$BL fs upload /sd/meshcore/sounds/2.wav  ./2.wav
$BL fs upload /sd/meshcore/sounds/3.wav  ./3.wav
$BL fs upload /sd/meshcore/sounds/4.wav  ./4.wav
```

> Argument order quirk: `badgelink fs upload` is **badge file first,
> host file second** — the opposite of `scp`. Easy to swap and get a
> `No such file or directory` error.

## Testing

In Settings → Sounds the bottom four rows are action-row previews:

- **Preview DM** — plays whichever slot DM points to
- **Preview channel** — same for channel
- **Preview error** — same for error
- **Preview boot** — same for boot

Press Enter on the row to play. If you don't hear anything, in order:

1. Check the volume row — `0%` mutes everything.
2. Check the event row — set to `Off` mutes only that event.
3. Run `Preview <event>` — bypasses the on/off flag, useful for "did
   the file even load" debugging.
4. Check the serial log for `sounds: ...` errors. Common ones:
   - `sounds: file not found /sd/meshcore/sounds/N.wav` — typo in the
     path or SD not mounted.
   - `sounds: unsupported format` — re-encode with the ffmpeg recipe
     above.
   - `sounds: bsp_audio not ready` — amplifier failed to power on; try
     a reboot.

## Why no bundled samples?

Licensing fragility. Pixabay's content licence is permissive but their
TOS allows individual creators to upload work with extra restrictions
(no AI training, attribution required, etc.), enforced per-clip. Same
goes for Freesound — most CC0, some CC-BY, a few CC-BY-NC. Bundling a
clip in this repo under our MIT licence would mean tracking each
upstream licence in lockstep with any clip rotation. Asking you to pick
your own keeps that responsibility where the law puts it.
