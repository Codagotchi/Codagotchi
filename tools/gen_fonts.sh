#!/bin/bash
# Generate LVGL 9 bitmap fonts from Noto Sans / Noto Sans Mono TTFs.
# Replaces the proprietary Tiempos + Styrene + DejaVu font .c files with
# Noto Sans (SIL Open Font License 1.1) equivalents at the same sizes.
#
# Usage: ./tools/gen_fonts.sh
# Requires: node, npm  (lv_font_conv installed automatically if missing)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
FONT_DIR="$REPO_DIR/assets/fonts"
OUT_DIR="$REPO_DIR/firmware/src"
PATCH="$SCRIPT_DIR/patch_lvgl9.js"

mkdir -p "$FONT_DIR"

# ── Download fonts ──────────────────────────────────────────────────────────

SANS_TTF="$FONT_DIR/NotoSans-Regular.ttf"
MONO_TTF="$FONT_DIR/NotoSansMono-Regular.ttf"

if [ ! -f "$SANS_TTF" ]; then
    echo "[1/3] Downloading Noto Sans Regular (SIL OFL)..."
    curl -fsSL "https://github.com/googlefonts/noto-fonts/raw/main/hinted/ttf/NotoSans/NotoSans-Regular.ttf" \
        -o "$SANS_TTF"
fi

if [ ! -f "$MONO_TTF" ]; then
    echo "[1/3] Downloading Noto Sans Mono Regular (SIL OFL)..."
    curl -fsSL "https://github.com/googlefonts/noto-fonts/raw/main/hinted/ttf/NotoSansMono/NotoSansMono-Regular.ttf" \
        -o "$MONO_TTF"
fi

# ── Install lv_font_conv ────────────────────────────────────────────────────

if ! command -v lv_font_conv &>/dev/null; then
    echo "[2/3] Installing lv_font_conv..."
    npm install -g lv_font_conv
fi

# ── Generate fonts ──────────────────────────────────────────────────────────

echo "[3/3] Generating fonts..."

# Standard ASCII printable range sufficient for all UI strings.
RANGE="0x20-0x7E"
BPP=4

gen() {
    local font="$1" size="$2" out="$3"
    printf "  %-22s %2dpx ... " "$out" "$size"
    lv_font_conv \
        --font "$font" \
        --size "$size" \
        --format lvgl \
        --bpp "$BPP" \
        -r "$RANGE" \
        --no-compress \
        --lv-include lvgl.h \
        -o "$OUT_DIR/$out"
    echo "ok"
}
# Note: lv_font_conv >= 4.x already emits LVGL 9-compatible multi-version
# conditionals (#if LVGL_VERSION_MAJOR >= 8 for const, .fallback, etc.).
# No post-patch needed. patch_lvgl9.js is kept for old-format files only.

# Tiempos slots (display / title / countdown) → Noto Sans
gen "$SANS_TTF" 56 font_tiempos_56.c
gen "$SANS_TTF" 34 font_tiempos_34.c

# Styrene slots (UI labels) → Noto Sans
gen "$SANS_TTF" 48 font_styrene_48.c
gen "$SANS_TTF" 28 font_styrene_28.c
gen "$SANS_TTF" 24 font_styrene_24.c
gen "$SANS_TTF" 20 font_styrene_20.c
gen "$SANS_TTF" 16 font_styrene_16.c
gen "$SANS_TTF" 14 font_styrene_14.c
gen "$SANS_TTF" 12 font_styrene_12.c

# Mono slots → Noto Sans Mono
gen "$MONO_TTF" 32 font_mono_32.c
gen "$MONO_TTF" 18 font_mono_18.c

echo ""
echo "Done. Build with:"
echo "  pio run -d firmware -e waveshare_amoled_216"
echo "  pio run -d firmware -e waveshare_amoled_18"
