#!/bin/bash
# fix-toolchain-c6-mac.sh
#
# One-time setup for building the waveshare_amoled_216_c6 firmware on macOS
# Apple Silicon (arm64).
#
# The pioarduino platform-espressif32 ships a broken RISC-V toolchain zip that
# is missing the GCC compiler. This script:
#   1. Checks for sufficient disk space (~3 GB needed)
#   2. Installs GCC 14.2.0 from Espressif's crosstool-NG release
#   3. Patches platform.json to stop PlatformIO re-downloading the wrong toolchain
#   4. Patches the toolchain's dirent.h so ESP-IDF's VFS layer compiles correctly
#
# Run once before your first flash:
#   ./fix-toolchain-c6-mac.sh
#
# After this, use flash-mac.sh as normal:
#   ./flash-mac.sh waveshare_amoled_216_c6

set -e

ARCH=$(uname -m)
if [ "$ARCH" != "arm64" ]; then
    echo "Error: this script is for Apple Silicon (arm64) only. Got: $ARCH"
    exit 1
fi

TOOLCHAIN_DIR="$HOME/.platformio/packages/toolchain-riscv32-esp"
PLATFORM_JSON="$HOME/.platformio/platforms/espressif32/platform.json"
GCC_VERSION="14.2.0_20260121"
GCC_URL="https://github.com/espressif/crosstool-NG/releases/download/esp-${GCC_VERSION}/riscv32-esp-elf-${GCC_VERSION}-aarch64-apple-darwin.tar.gz"

echo "=== Codagotchi C6 toolchain fix ==="
echo ""

# ── 1. Disk space check ────────────────────────────────────────────────────────
AVAIL_GB=$(df -g / | awk 'NR==2 {print $4}')
if [ "$AVAIL_GB" -lt 3 ]; then
    echo "Error: need at least 3 GB free, only ${AVAIL_GB} GB available."
    echo "Free up space and re-run. Tip: the Xtensa toolchain is safe to remove"
    echo "if you only build for the C6 board:"
    echo "  rm -rf ~/.platformio/packages/toolchain-xtensa-esp-elf"
    echo "  rm -rf ~/.platformio/packages/tool-xtensa-esp-elf-gdb"
    exit 1
fi
echo "✓ Disk space OK (${AVAIL_GB} GB free)"

# ── 2. Install GCC 14.2.0 ─────────────────────────────────────────────────────
echo ""
echo "Downloading GCC ${GCC_VERSION} for RISC-V (Apple Silicon)..."
echo "  Source: $GCC_URL"
echo "  (~560 MB download)"
echo ""

rm -rf "$TOOLCHAIN_DIR"

curl -L --progress-bar "$GCC_URL" \
    | tar -xz -C "$HOME/.platformio/packages/"

mv "$HOME/.platformio/packages/riscv32-esp-elf" "$TOOLCHAIN_DIR"

INSTALLED_VER=$("$TOOLCHAIN_DIR/bin/riscv32-esp-elf-g++" --version | head -1)
echo ""
echo "✓ Installed: $INSTALLED_VER"

# ── 3. Patch platform.json ────────────────────────────────────────────────────
echo ""
echo "Patching platform.json..."

if [ ! -f "$PLATFORM_JSON" ]; then
    echo "Error: $PLATFORM_JSON not found."
    echo "Run 'pio run -e waveshare_amoled_216_c6' once first so PlatformIO"
    echo "downloads the platform, then re-run this script."
    exit 1
fi

python3 - "$PLATFORM_JSON" << 'PYEOF'
import json, sys
path = sys.argv[1]
with open(path) as f:
    d = json.load(f)
d['packages']['toolchain-riscv32-esp'] = {
    'type': 'toolchain',
    'optional': True,
    'owner': 'pioarduino',
    'package-version': '14.2.0+20260121',
    'version': 'https://github.com/pioarduino/registry/releases/download/0.0.1/riscv32-esp-elf-14.2.0_20260121.zip'
}
with open(path, 'w') as f:
    json.dump(d, f, indent=2)
PYEOF

echo "✓ platform.json patched"

# ── 4. Patch dirent.h ─────────────────────────────────────────────────────────
echo ""
echo "Patching toolchain dirent.h..."

SYSROOT="$TOOLCHAIN_DIR/riscv32-esp-elf"

# Disable the sysroot sys-include directory — it has headers that intercept
# ESP-IDF's VFS includes before they can be resolved
if [ -d "$SYSROOT/sys-include" ]; then
    mv "$SYSROOT/sys-include" "$SYSROOT/sys-include-disabled"
fi

# Replace include/sys/dirent.h with an ESP-IDF-compatible stub.
# The RISC-V newlib doesn't support directory operations (opendir/readdir/DIR),
# but ESP-IDF's VFS layer implements them — we just need the declarations.
cat > "$SYSROOT/include/sys/dirent.h" << 'EOF'
/* sys/dirent.h — ESP-IDF VFS compatibility shim for RISC-V newlib.
   ESP-IDF implements opendir/readdir/closedir through its VFS layer.
   This stub provides the required type declarations. */
#ifndef _SYS_DIRENT_H_
#define _SYS_DIRENT_H_
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
#define DT_UNKNOWN  0
#define DT_REG      1
#define DT_DIR      2
#define DT_LNK      3
#define DT_FIFO     4
#define DT_SOCK     5
#define DT_CHR      6
#define DT_BLK      7
struct dirent { uint8_t d_type; char d_name[256]; };
typedef struct { uint32_t _private[8]; } DIR;
DIR            *opendir(const char *);
struct dirent  *readdir(DIR *);
int             readdir_r(DIR * __restrict, struct dirent * __restrict, struct dirent ** __restrict);
void            rewinddir(DIR *);
void            seekdir(DIR *, long);
long            telldir(DIR *);
int             closedir(DIR *);
int             dirfd(DIR *);
#ifdef __cplusplus
}
#endif
#endif
EOF

echo "✓ dirent.h patched"

echo ""
echo "=== Done ==="
echo ""
echo "You can now flash the C6 firmware:"
echo "  ./flash-mac.sh waveshare_amoled_216_c6"
