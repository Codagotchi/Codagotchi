#!/usr/bin/env python3
"""Render a Codex pet's idle frame in the terminal as ANSI 24-bit color.

Uses the half-block trick (each terminal cell shows two vertical pixels via
the ▀ character with separate fg/bg colors) so the aspect ratio stays
roughly 1:1 per cell.

Drop-in for a future launcher script: spit one of these next to each pet
the user can pick, then prompt for a choice and feed it to pet_to_lvgl.py.

Usage:
    python3 tools/preview_pet.py <pet-dir> [--width CELLS]
"""

import argparse
import sys
from pathlib import Path

from PIL import Image

CELL_W, CELL_H = 192, 208


def render(pet_dir: Path, width: int, bg: tuple[int, int, int]) -> str:
    atlas = Image.open(pet_dir / "spritesheet.webp").convert("RGBA")
    cell = atlas.crop((0, 0, CELL_W, CELL_H))
    # Aspect-preserving height in pixels, rounded to an even number so we
    # can pair rows for the half-block.
    h = round(width * CELL_H / CELL_W)
    if h % 2:
        h += 1
    cell = cell.resize((width, h), Image.LANCZOS)
    px = cell.load()
    br, bgc, bb = bg

    def composite(p):
        r, g, b, a = p
        if a == 0:
            return (br, bgc, bb)
        if a < 255:
            inv = 255 - a
            return (
                (r * a + br * inv) // 255,
                (g * a + bgc * inv) // 255,
                (b * a + bb * inv) // 255,
            )
        return (r, g, b)

    RESET = "\x1b[0m"
    out = []
    for y in range(0, h, 2):
        line = []
        for x in range(width):
            tr, tg, tb = composite(px[x, y])
            br2, bg2, bb2 = composite(px[x, y + 1])
            line.append(f"\x1b[38;2;{tr};{tg};{tb};48;2;{br2};{bg2};{bb2}m▀")
        line.append(RESET)
        out.append("".join(line))
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("pet_dir", type=Path)
    ap.add_argument("--width", type=int, default=32, help="output width in terminal cells")
    ap.add_argument("--bg", default="000000", help="composite background as RRGGBB hex")
    args = ap.parse_args()
    bg = (int(args.bg[0:2], 16), int(args.bg[2:4], 16), int(args.bg[4:6], 16))
    print(render(args.pet_dir, args.width, bg))
    return 0


if __name__ == "__main__":
    sys.exit(main())
