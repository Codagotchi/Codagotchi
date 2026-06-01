#!/usr/bin/env python3
"""Convert a Codex pet folder (pet.json + spritesheet.webp) into a C header of
RGB565A8 frame arrays consumable by LVGL 9.

Codex pet atlas spec (from openai/skills/hatch-pet SKILL.md):
  - 8 columns x 9 rows = 72 cells
  - Cell size 192 x 208, transparent background
  - Row order: idle, running-right, running-left, waving, jumping,
               failed, waiting, running, review

We currently extract only `idle` (row 0) and `running` (row 7), since those
are the only two states the firmware driver maps onto (Codex `ac` field).

Output header layout:
  #define CODEX_PET_FRAME_W <out_w>
  #define CODEX_PET_FRAME_H <out_h>
  #define CODEX_PET_FRAMES_PER_STATE 8
  static const uint8_t codex_pet_idle_<i>_data[w*h*3]    { ... };  // 8 frames
  static const uint8_t codex_pet_running_<i>_data[w*h*3] { ... };  // 8 frames

Usage:
  python3 tools/pet_to_lvgl.py ~/.codex/pets/crittersquest \\
      --out firmware/src/codex_pet_frames.h --scale 0.5
"""

import argparse
import json
import sys
from pathlib import Path

from PIL import Image

# Row order from the hatch-pet SKILL.md spec.
STATE_ROWS = [
    "idle", "running-right", "running-left", "waving", "jumping",
    "failed", "waiting", "running", "review",
]
CELL_W = 192
CELL_H = 208
COLS = 8
ROWS = 9

# We bake these two states; mapped onto Codex `ac` true/false at runtime.
# The firmware refers to the second one as `codex_pet_running_frames` — see
# the explicit rename in build_header() below.
WANTED_STATES = ["idle", "running-right"]
# Map source row name → C symbol stem used by the firmware. Keeping the
# firmware ignorant of which source row it came from lets us swap (e.g.
# running ↔ running-right ↔ running-left) without touching the device code.
STATE_SYMBOL_RENAMES = {"running-right": "running", "running-left": "running"}


def rgba_to_rgb565(im: Image.Image, bg_rgb: tuple[int, int, int]) -> bytes:
    """Pre-composite RGBA pixels against `bg_rgb` and pack as little-endian RGB565.

    Returns w*h*2 bytes — no alpha plane. The firmware draws these as plain
    RGB565 images, which on slow chips (ESP32-C6) avoids the per-strip
    alpha-blend pass that was causing visible flicker between frame swaps.
    The tradeoff is the frames are baked against the theme background — if
    the screen bg color changes, you'll see a hard-edged rectangle around
    the pet. We use THEME_BG (black) so the seam is invisible.
    """
    if im.mode != "RGBA":
        im = im.convert("RGBA")
    px = im.load()
    w, h = im.size
    out = bytearray(w * h * 2)
    bgr, bgg, bgb = bg_rgb
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            # Alpha-over: out = src*a + bg*(1-a)
            if a == 0:
                r, g, b = bgr, bgg, bgb
            elif a < 255:
                inv = 255 - a
                r = (r * a + bgr * inv) // 255
                g = (g * a + bgg * inv) // 255
                b = (b * a + bgb * inv) // 255
            v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            out[i * 2]     = v & 0xFF
            out[i * 2 + 1] = (v >> 8) & 0xFF
            i += 1
    return bytes(out)


def dominant_color(cell: Image.Image) -> tuple[int, int, int]:
    """Pick a representative non-bg, non-glint color from a cell.

    Strategy: take opaque pixels (alpha>200), drop near-black and near-white,
    then return the most common remaining color. That's typically the pet's
    body color rather than eye glints or background bleed.
    """
    if cell.mode != "RGBA":
        cell = cell.convert("RGBA")
    from collections import Counter
    counts: Counter = Counter()
    for r, g, b, a in cell.getdata():
        if a < 200:
            continue
        s = r + g + b
        if s < 120 or s > 720:  # exclude near-black and near-white
            continue
        counts[(r, g, b)] += 1
    if not counts:
        return (128, 128, 128)
    return counts.most_common(1)[0][0]


def cell_is_empty(cell: Image.Image) -> bool:
    """Treat a cell as empty if essentially no pixel is opaque (alpha > 32).

    The hatch-pet spec allows a row to have fewer than 8 frames — unused
    cells are transparent padding. If we don't skip them the pet visibly
    blinks off at the trailing frames of every loop.
    """
    if cell.mode != "RGBA":
        cell = cell.convert("RGBA")
    alphas = cell.getchannel("A").getdata()
    return sum(1 for a in alphas if a > 32) < 16  # tolerate ~stray pixels


def extract_frames(
    atlas: Image.Image,
    row_idx: int,
    out_size: tuple[int, int],
    bg_rgb: tuple[int, int, int],
) -> list[bytes]:
    """Crop frames from one row, drop trailing empties, scale + composite, return RGB565 blobs."""
    aw, ah = atlas.size
    assert aw == COLS * CELL_W and ah == ROWS * CELL_H, (
        f"atlas is {aw}x{ah}, expected {COLS * CELL_W}x{ROWS * CELL_H}"
    )
    y0 = row_idx * CELL_H
    cells = []
    for col in range(COLS):
        x0 = col * CELL_W
        cell = atlas.crop((x0, y0, x0 + CELL_W, y0 + CELL_H))
        cells.append(cell)
    # Drop trailing empty cells (the typical hatch-pet layout for shorter
    # animations). We only trim from the end so an intermediate transparent
    # frame — if it existed — would still be kept.
    while cells and cell_is_empty(cells[-1]):
        cells.pop()
    blobs = []
    for cell in cells:
        if cell.size != out_size:
            cell = cell.resize(out_size, Image.LANCZOS)
        blobs.append(rgba_to_rgb565(cell, bg_rgb))
    return blobs


def render_blob_c_array(name: str, blob: bytes) -> str:
    """Format a uint8_t array literal — 16 bytes per line."""
    lines = [f"static const uint8_t {name}[{len(blob)}] = {{"]
    for i in range(0, len(blob), 16):
        chunk = blob[i : i + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pet_dir", type=Path, help="folder containing pet.json + spritesheet.webp")
    ap.add_argument("--out", type=Path, default=Path("firmware/src/codex_pet_frames.h"),
                    help="output C header path")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="default scale factor applied to each 192x208 cell")
    ap.add_argument("--idle-scale", type=float, default=None,
                    help="scale override for the idle state (defaults to --scale)")
    ap.add_argument("--running-scale", type=float, default=None,
                    help="scale override for the running state (defaults to --scale)")
    ap.add_argument("--bg", default="000000",
                    help="RRGGBB background color to pre-composite alpha against (default 000000 — THEME_BG)")
    args = ap.parse_args()
    bg_rgb = (int(args.bg[0:2], 16), int(args.bg[2:4], 16), int(args.bg[4:6], 16))
    state_scales = {
        "idle":          args.idle_scale    if args.idle_scale    is not None else args.scale,
        "running-right": args.running_scale if args.running_scale is not None else args.scale,
        "running-left":  args.running_scale if args.running_scale is not None else args.scale,
        "running":       args.running_scale if args.running_scale is not None else args.scale,
    }

    meta_path = args.pet_dir / "pet.json"
    atlas_path = args.pet_dir / "spritesheet.webp"
    if not meta_path.exists() or not atlas_path.exists():
        print(f"Missing pet.json or spritesheet.webp in {args.pet_dir}", file=sys.stderr)
        return 1

    meta = json.loads(meta_path.read_text())
    pet_id = meta.get("id", args.pet_dir.name)
    pet_name = meta.get("displayName", pet_id)
    description = meta.get("description", "")

    atlas = Image.open(atlas_path).convert("RGBA")
    print(f"Pet:   {pet_name} ({pet_id})")
    print(f"Atlas: {atlas.size[0]}x{atlas.size[1]}")

    # Sample the idle row's first frame to pick the pet's signature color —
    # used by the firmware for the usage ring around the idle pet so the
    # accent matches the character instead of the screen's orange theme.
    idle_cell = atlas.crop((0, 0, CELL_W, CELL_H))
    cr, cg, cb = dominant_color(idle_cell)
    print(f"Color: #{cr:02X}{cg:02X}{cb:02X}")

    # Per-state output dimensions. Idle can render small, running can render
    # big — gives the user a clear "Codex is working" cue without changing
    # the screen layout.
    state_dims: dict[str, tuple[int, int]] = {}  # symbol -> (w, h)
    pointer_lists: dict[str, list[str]] = {}
    total_bytes = 0
    state_blob_arrays: list[str] = []

    for state in WANTED_STATES:
        scale = state_scales.get(state, args.scale)
        out_w = int(round(CELL_W * scale))
        out_h = int(round(CELL_H * scale))
        # Force even width so RGB565 row stride is 4-byte aligned (LVGL prefers this).
        if out_w % 2:
            out_w -= 1
        row_idx = STATE_ROWS.index(state)
        frames = extract_frames(atlas, row_idx, (out_w, out_h), bg_rgb)
        if not frames:
            # Fallback so the firmware always has at least one frame per state.
            frames = extract_frames(atlas, STATE_ROWS.index("idle"), (out_w, out_h), bg_rgb)
        symbol = STATE_SYMBOL_RENAMES.get(state, state)
        state_dims[symbol] = (out_w, out_h)
        names = []
        for i, blob in enumerate(frames):
            varname = f"codex_pet_{symbol}_{i}_data"
            state_blob_arrays.append(render_blob_c_array(varname, blob))
            state_blob_arrays.append("")
            names.append(varname)
            total_bytes += len(blob)
        pointer_lists[symbol] = names
        print(f"  {state:14s} -> codex_pet_{symbol}_*: {out_w}x{out_h}, {len(frames)} frames")

    header_lines = [
        "// AUTO-GENERATED by tools/pet_to_lvgl.py — DO NOT EDIT.",
        f"// Pet: {pet_name} ({pet_id})",
        f"// {description}",
        f"// Plain RGB565 (no alpha) — pet pixels pre-composited against bg #{args.bg}.",
        "// Trailing transparent cells (hatch-pet padding) are trimmed at convert",
        "// time so the device doesn't render blank frames between visible poses.",
        "// Per-state W/H lets idle render small and running render big.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define CODEX_PET_NAME \"{pet_name}\"",
        f"#define CODEX_PET_COLOR_HEX 0x{cr:02X}{cg:02X}{cb:02X}",
        "",
    ]
    for symbol, (w, h) in state_dims.items():
        header_lines.append(f"#define CODEX_PET_{symbol.upper()}_W {w}")
        header_lines.append(f"#define CODEX_PET_{symbol.upper()}_H {h}")
    for symbol, names in pointer_lists.items():
        header_lines.append(f"#define CODEX_PET_{symbol.upper()}_FRAMES {len(names)}")
    header_lines.append("")
    header_lines.extend(state_blob_arrays)

    for symbol, names in pointer_lists.items():
        header_lines.append(
            f"static const uint8_t* const codex_pet_{symbol}_frames[{len(names)}] = {{"
        )
        header_lines.append("    " + ", ".join(names))
        header_lines.append("};")
        header_lines.append("")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(header_lines))
    print(f"Wrote {args.out} ({total_bytes:,} bytes of frame data)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
