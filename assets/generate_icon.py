#!/usr/bin/env python3
"""Regenerate the MeshCore app icon (concept A: 4-node mesh).

Three concepts were originally generated for selection (A=mesh, B=LoRa+M,
C=concentric). Concept A was chosen and shipped as `icon-32.png` /
`icon-256.png` next to this script.

Run from this directory:

    python3 generate_icon.py

All use Tokyo Night palette (matches the in-app theme):
  bg     = #1A1B26
  accent = #7AA2F7  (blue)
  text   = #C0CAF5  (off-white)
  amber  = #E0AF68
  green  = #9ECE6A
"""
import os
from PIL import Image, ImageDraw

OUT = os.path.dirname(os.path.abspath(__file__))

BG     = (26, 27, 38, 255)
ACCENT = (122, 162, 247, 255)
TEXT   = (192, 202, 245, 255)
AMBER  = (224, 175, 104, 255)
GREEN  = (158, 206, 106, 255)
DIM    = (86, 95, 137, 255)

SIZE = 32
# Also render a 256x256 preview so the concepts can be inspected easily.
PREV = 256


def new_canvas(s):
    im = Image.new("RGBA", (s, s), BG)
    return im, ImageDraw.Draw(im, "RGBA")


def draw_dot(d, cx, cy, r, color):
    d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color)


def concept_mesh(im, d, s):
    """A: mesh-network visual — 4 nodes connected, center node accent."""
    # Background rounded square
    pad = max(1, s // 16)
    d.rounded_rectangle((pad, pad, s - pad, s - pad), radius=s // 8,
                        fill=(36, 40, 59, 255))

    # 4 corner nodes + 1 center
    cx, cy = s // 2, s // 2
    r_outer = (s - 2 * pad) // 2 - max(2, s // 12)
    nodes = [
        (cx, cy - r_outer + s // 12),  # top
        (cx + r_outer - s // 12, cy),  # right
        (cx, cy + r_outer - s // 12),  # bottom
        (cx - r_outer + s // 12, cy),  # left
    ]
    node_r = max(2, s // 12)
    line_w = max(1, s // 32)

    # Edges from center to each
    for nx, ny in nodes:
        d.line((cx, cy, nx, ny), fill=DIM, width=line_w)
    # Some peripheral edges for "mesh" feel
    d.line(nodes[0] + nodes[1], fill=DIM, width=line_w)
    d.line(nodes[1] + nodes[2], fill=DIM, width=line_w)

    # Peripheral nodes
    for nx, ny in nodes:
        draw_dot(d, nx, ny, node_r, ACCENT)
        draw_dot(d, nx, ny, max(1, node_r - max(1, s // 32)), TEXT)
    # Center node bigger + amber
    draw_dot(d, cx, cy, node_r + max(1, s // 24), AMBER)
    draw_dot(d, cx, cy, node_r - max(1, s // 32), TEXT)


def concept_lora_m(im, d, s):
    """B: stylised 'M' with a radio-wave arc."""
    pad = max(1, s // 16)
    d.rounded_rectangle((pad, pad, s - pad, s - pad), radius=s // 8,
                        fill=(36, 40, 59, 255))

    # Three arcs (radio waves) from top-right.
    cx_w = int(s * 0.78)
    cy_w = int(s * 0.22)
    for k, col in enumerate([DIM, ACCENT, TEXT]):
        rr = max(3, s // 4) + k * max(1, s // 12)
        d.arc((cx_w - rr, cy_w - rr, cx_w + rr, cy_w + rr),
              start=200, end=290, fill=col, width=max(1, s // 28))

    # Letter M body
    bx1 = int(s * 0.18)
    bx2 = int(s * 0.82)
    by1 = int(s * 0.38)
    by2 = int(s * 0.80)
    bw = max(2, s // 10)
    # Left vertical
    d.rectangle((bx1, by1, bx1 + bw, by2), fill=TEXT)
    # Right vertical
    d.rectangle((bx2 - bw, by1, bx2, by2), fill=TEXT)
    # V-shape in middle
    mid_x = (bx1 + bx2) // 2
    mid_y = int(by1 + (by2 - by1) * 0.55)
    d.line((bx1 + bw // 2, by1, mid_x, mid_y), fill=TEXT, width=bw)
    d.line((bx2 - bw // 2, by1, mid_x, mid_y), fill=TEXT, width=bw)


def concept_minimal(im, d, s):
    """C: minimal Tanmatsu-style mark — concentric hexagonal/circular waves
    with a single accent dot, no text."""
    pad = max(1, s // 16)
    d.rounded_rectangle((pad, pad, s - pad, s - pad), radius=s // 8,
                        fill=(36, 40, 59, 255))

    cx, cy = s // 2, s // 2
    # Three concentric arcs as broadcasting waves
    for k, col in enumerate([DIM, DIM, ACCENT]):
        rr = max(2, s // 8) + k * max(1, s // 8)
        # Almost-full circle, slight gap at bottom
        d.arc((cx - rr, cy - rr, cx + rr, cy + rr),
              start=200, end=520, fill=col, width=max(1, s // 28))
    # Centre dot bigger, amber accent
    draw_dot(d, cx, cy, max(2, s // 10), AMBER)
    draw_dot(d, cx, cy, max(1, s // 16), TEXT)


def render_native_and_hires(out_native, out_hires, fn):
    """Write the chosen concept at the launcher's 32x32 tile size and a
    high-res native render for store listings / marketing."""
    im32, d32 = new_canvas(SIZE)
    fn(im32, d32, SIZE)
    im32.save(out_native)

    imN, dN = new_canvas(PREV)
    fn(imN, dN, PREV)
    imN.save(out_hires)


# Shipping: concept A (mesh-network).
render_native_and_hires(f"{OUT}/icon-32.png", f"{OUT}/icon-256.png", concept_mesh)
print(f"wrote {OUT}/icon-32.png + icon-256.png")

# Uncomment to re-evaluate alternate concepts in a fresh side-by-side review:
# for name, fn in [("a_mesh", concept_mesh),
#                  ("b_lora_m", concept_lora_m),
#                  ("c_minimal", concept_minimal)]:
#     render_native_and_hires(f"{OUT}/{name}_32.png", f"{OUT}/{name}_hi.png", fn)
