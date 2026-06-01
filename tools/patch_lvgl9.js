#!/usr/bin/env node
// Converts lv_font_conv LVGL 8 output to LVGL 9 format.
// Usage: node patch_lvgl9.js <file.c> [out.c]   (in-place if out omitted)
//
// Changes made:
//   - Remove #if LVGL_VERSION_MAJOR >= 8 / #endif guards around font struct
//   - Drop .cache field if present
//   - Insert .release_glyph, .kerning, .static_bitmap after .subpx
//   - Insert .fallback before .user_data

const fs = require('fs');

const inputFile = process.argv[2];
const outputFile = process.argv[3] || inputFile;

if (!inputFile) {
    console.error('Usage: node patch_lvgl9.js <file.c> [out.c]');
    process.exit(1);
}

let src = fs.readFileSync(inputFile, 'utf8');

// Remove version guard opening line
src = src.replace(/#if LVGL_VERSION_MAJOR >= 8\n/g, '');
// Remove the closing #endif that immediately follows the font struct (};)
src = src.replace(/(\};\n)#endif\b[^\n]*\n/g, '$1');

// Drop .cache field (LVGL 8 had it, LVGL 9 removed it)
src = src.replace(/[ \t]*\.cache\s*=\s*\{[^}]*\},?[ \t]*\n/g, '');

// Insert LVGL 9 fields after .subpx line (idempotent)
if (!src.includes('.release_glyph')) {
    src = src.replace(
        /([ \t]*\.subpx\s*=\s*LV_FONT_SUBPX_NONE,[^\n]*\n)/,
        '$1    .release_glyph = NULL,\n    .kerning = 0,\n    .static_bitmap = 0,\n'
    );
}

// Insert .fallback before .user_data (idempotent)
if (!src.includes('.fallback')) {
    src = src.replace(
        /([ \t]*\.user_data\s*=\s*NULL)/,
        '    .fallback = NULL,\n$1'
    );
}

fs.writeFileSync(outputFile, src);
if (process.argv[3]) {
    console.log(`  patched → ${outputFile}`);
}
