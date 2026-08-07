#!/usr/bin/env python3
# Regenerates components/vendor/emoji_bitmaps.c from the Twemoji 72x72 PNGs.
#
# The names and codepoints come from EMOJI_SET in mc_common/emoji_table.c, which
# stays the source of truth for what the picker shows. Twemoji art is CC-BY 4.0
# (Twitter, now jdecked); the downscale is Pillow LANCZOS, which is what the
# committed file was produced with and reproduces it byte for byte.
#
#   pip install pillow
#   scripts/gen_emoji_bitmaps.py > components/vendor/emoji_bitmaps.c
#   scripts/gen_emoji_bitmaps.py --check     # verify, write nothing
import argparse
import io
import re
import sys
import urllib.request
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
TABLE = ROOT / "components/mc_common/emoji_table.c"
TARGET = ROOT / "components/vendor/emoji_bitmaps.c"
CDN = "https://raw.githubusercontent.com/jdecked/twemoji/main/assets/72x72/{cp}.png"
SIZE = 32

HEADER = """// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Emoji 32x32 ARGB8888 bitmaps (generated). Twemoji CC-BY 4.0 (Twitter/jdecked).
//
// Do not hand-edit: regenerate with scripts/gen_emoji_bitmaps.py after changing
// EMOJI_SET in components/mc_common/emoji_table.c.

#include <stdint.h>

#define EMOJI_BITMAP_SIZE 32
"""


def emoji_set():
    rows = re.findall(r'\{0x([0-9A-Fa-f]+),\s*"[^"]*",\s*\d+\},\s*//\s*(\S+)', TABLE.read_text())
    if not rows:
        sys.exit(f"no EMOJI_SET rows found in {TABLE}")
    return [(int(cp, 16), name) for cp, name in rows]


def fetch(cp):
    url = CDN.format(cp=f"{cp:x}")
    req = urllib.request.Request(url, headers={"User-Agent": "gen_emoji_bitmaps"})
    try:
        return urllib.request.urlopen(req, timeout=60).read()
    except Exception as e:
        sys.exit(f"U+{cp:04X}: {url} -> {e}")


def bitmap(cp):
    im = Image.open(io.BytesIO(fetch(cp))).convert("RGBA")
    small = im.resize((SIZE, SIZE), Image.LANCZOS)
    return [(a << 24) | (r << 16) | (g << 8) | b for r, g, b, a in small.get_flattened_data()]


def render():
    out = [HEADER]
    for cp, name in emoji_set():
        px = bitmap(cp)
        out.append(f"\n// U+{cp:04X} ({name})")
        out.append(f"const uint32_t emoji_bitmap_{name}[32*32] = {{")
        for row in range(SIZE):
            vals = ", ".join(f"0x{v:08X}" for v in px[row * SIZE:(row + 1) * SIZE])
            out.append(f"    {vals},")
        out.append("};")
    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="compare against the committed file")
    args = ap.parse_args()
    text = render()
    if not args.check:
        sys.stdout.write(text)
        return 0
    current = TARGET.read_text()
    cur_px = re.findall(r"0x([0-9A-Fa-f]{8})", current)
    new_px = re.findall(r"0x([0-9A-Fa-f]{8})", text)
    if cur_px == new_px:
        print(f"emoji_bitmaps: {len(new_px)} pixels match the committed file")
        return 0
    diff = sum(1 for a, b in zip(cur_px, new_px) if a != b)
    print(f"emoji_bitmaps: MISMATCH ({diff} of {len(new_px)} pixels, "
          f"{len(cur_px)} committed vs {len(new_px)} generated)")
    return 1


if __name__ == "__main__":
    sys.exit(main())
