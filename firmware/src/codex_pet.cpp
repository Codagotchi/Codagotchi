#include "codex_pet.h"
#include "codex_pet_frames.h"
#include "theme.h"
#include "hal/board_caps.h"
#include <Arduino.h>

// Bigger frames don't fit in an lv_canvas RAM buffer on the C6 (320 KB
// internal SRAM), so we switched to lv_image. lv_image reads pixels
// directly from flash via its descriptor — no canvas buffer, no per-frame
// memcpy.
//
// Frame swap MUST be driven from the main loop AFTER lv_timer_handler (see
// codex_pet_tick called from main.cpp). Calling lv_image_set_src from
// inside an lv_timer callback races LVGL's render pass and brings back the
// flicker we already debugged out.
//
// Two prerequisites taught us in the painful path here:
//   1. Trailing transparent cells in the spritesheet are trimmed at convert
//      time (CODEX_PET_*_FRAMES counts the real frames only) — otherwise
//      the pet visibly blinks off for ~25% of every loop.
//   2. Pet pixels are pre-composited against THEME_BG at convert time so
//      the renderer doesn't have to alpha-blend per strip (slow on C6).

#define FRAME_MS 150  // ~6.7 fps

static lv_obj_t* s_container = nullptr;
static lv_obj_t* s_img       = nullptr;
static lv_obj_t* s_arc       = nullptr;  // 5h-window usage ring around the idle pet
static lv_obj_t* s_lbl_pct   = nullptr;  // "X%" below the pet
static lv_obj_t* s_lbl_reset = nullptr;  // "Resets in ..." below the pct
static lv_image_dsc_t s_idle_dscs[CODEX_PET_IDLE_FRAMES];
static lv_image_dsc_t s_running_dscs[CODEX_PET_RUNNING_FRAMES];
static lv_image_dsc_t* s_active_set = s_idle_dscs;
static uint8_t s_active_count = CODEX_PET_IDLE_FRAMES;
static uint8_t s_frame_idx = 0;
static uint32_t s_last_frame_ms = 0;
static bool s_visible = false;
static bool s_running_active = false;

// Mini idle-pet that replaces the brand logo in the Codex usage screen corner.
// Independent of the full-screen pet above — its own frame index/timer/visibility.
static lv_obj_t* s_mini_img       = nullptr;
static bool      s_mini_visible   = false;
static uint8_t   s_mini_frame_idx = 0;
static uint32_t  s_mini_last_ms   = 0;

// Layout constants for the idle-state ring + labels. The ring is sized to
// comfortably wrap the 192×208 idle pet; labels sit below the pet+ring with
// enough space for the 480-tall panel.
#define RING_DIAMETER  340       // bigger circle → more breathing room around the pet
#define RING_WIDTH     14
#define RING_Y_OFFSET  -40       // shift the whole pet+ring up so labels fit cleanly below
// Distance from screen center → label baseline. Labels live below the ring,
// so these are (ring_radius + offset + extra padding).
#define LBL_PCT_Y      (RING_DIAMETER / 2 + RING_Y_OFFSET + 50)
#define LBL_RESET_Y    (RING_DIAMETER / 2 + RING_Y_OFFSET + 86)

LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);

static void fill_dsc(lv_image_dsc_t* d, const uint8_t* data, int w, int h) {
    d->header.w     = w;
    d->header.h     = h;
    d->header.cf    = LV_COLOR_FORMAT_RGB565;
    d->header.stride = w * 2;
    d->data         = data;
    d->data_size    = w * h * 2;
}

void codex_pet_init(lv_obj_t* parent) {
    for (int i = 0; i < CODEX_PET_IDLE_FRAMES; i++)
        fill_dsc(&s_idle_dscs[i], codex_pet_idle_frames[i],
                 CODEX_PET_IDLE_W, CODEX_PET_IDLE_H);
    for (int i = 0; i < CODEX_PET_RUNNING_FRAMES; i++)
        fill_dsc(&s_running_dscs[i], codex_pet_running_frames[i],
                 CODEX_PET_RUNNING_W, CODEX_PET_RUNNING_H);

    const int W = board_caps().width;
    const int H = board_caps().height;

    s_container = lv_obj_create(parent);
    lv_obj_set_size(s_container, W, H);
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_set_style_bg_color(s_container, THEME_BG, 0);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);

    // Ring (lv_arc) sits BEHIND the pet — created first so it's lower in
    // z-order. Arc color is the pet's signature color from the spritesheet.
    s_arc = lv_arc_create(s_container);
    lv_obj_set_size(s_arc, RING_DIAMETER, RING_DIAMETER);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_arc_set_bg_angles(s_arc, 270, 270 - 1);  // full 360° track
    lv_arc_set_rotation(s_arc, 270);            // start at 12 o'clock
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, RING_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(CODEX_PET_COLOR_HEX), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_arc, RING_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_arc, 0, 0);
    lv_obj_set_style_pad_all(s_arc, 0, 0);
    // Hide the draggable knob; don't intercept clicks.
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_arc, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_align(s_arc, LV_ALIGN_CENTER, 0, RING_Y_OFFSET);

    s_img = lv_image_create(s_container);
    lv_image_set_src(s_img, &s_idle_dscs[0]);
    // Align (not center) — idle and running frames have different sizes, so
    // we need re-centering whenever the src changes. lv_obj_align with
    // LV_ALIGN_CENTER is sticky: LVGL re-applies it on layout changes.
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, RING_Y_OFFSET);  // co-centered with the ring
    // Bubble clicks up to the container so ui.cpp's global cycle handler fires.
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_CLICKABLE);

    // "X% used" label below the ring (white, small).
    s_lbl_pct = lv_label_create(s_container);
    lv_label_set_text(s_lbl_pct, "--% used");
    lv_obj_set_style_text_font(s_lbl_pct, &font_styrene_28, 0);
    lv_obj_set_style_text_color(s_lbl_pct, THEME_TEXT, 0);
    lv_obj_align(s_lbl_pct, LV_ALIGN_CENTER, 0, LBL_PCT_Y);

    // Reset countdown label below (white, slightly smaller).
    s_lbl_reset = lv_label_create(s_container);
    lv_label_set_text(s_lbl_reset, "---");
    lv_obj_set_style_text_font(s_lbl_reset, &font_styrene_24, 0);
    lv_obj_set_style_text_color(s_lbl_reset, THEME_TEXT, 0);
    lv_obj_align(s_lbl_reset, LV_ALIGN_CENTER, 0, LBL_RESET_Y);

    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
}

void codex_pet_show(void) {
    if (!s_container) return;
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    s_last_frame_ms = millis();
}

void codex_pet_hide(void) {
    if (!s_container) return;
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

void codex_pet_tick(void) {
    uint32_t now = millis();
    // Full-screen pet (idle or running set).
    if (s_visible && s_img && s_active_count > 0 && now - s_last_frame_ms >= FRAME_MS) {
        s_last_frame_ms = now;
        s_frame_idx = (s_frame_idx + 1) % s_active_count;
        lv_image_set_src(s_img, &s_active_set[s_frame_idx]);
    }
    // Mini corner pet (always idle frames) on the Codex usage screen.
    if (s_mini_visible && s_mini_img && now - s_mini_last_ms >= FRAME_MS) {
        s_mini_last_ms = now;
        s_mini_frame_idx = (s_mini_frame_idx + 1) % CODEX_PET_IDLE_FRAMES;
        lv_image_set_src(s_mini_img, &s_idle_dscs[s_mini_frame_idx]);
    }
}

void codex_pet_set_active(bool active) {
    if (active == s_running_active) return;
    s_running_active = active;
    if (active) {
        s_active_set   = s_running_dscs;
        s_active_count = CODEX_PET_RUNNING_FRAMES;
    } else {
        s_active_set   = s_idle_dscs;
        s_active_count = CODEX_PET_IDLE_FRAMES;
    }
    s_frame_idx = 0;
    if (s_img) {
        lv_image_set_src(s_img, &s_active_set[0]);
        // Idle frame is offset up so labels fit below. Running frame is
        // big — just center it on the screen.
        lv_obj_align(s_img, LV_ALIGN_CENTER, 0, active ? 0 : RING_Y_OFFSET);
    }
    // Hide ring + labels while running — the big pet uses that area.
    if (s_arc) {
        if (active) lv_obj_add_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_lbl_pct) {
        if (active) lv_obj_add_flag(s_lbl_pct, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(s_lbl_pct, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_lbl_reset) {
        if (active) lv_obj_add_flag(s_lbl_reset, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(s_lbl_reset, LV_OBJ_FLAG_HIDDEN);
    }
}

void codex_pet_set_usage(int pct, int reset_mins) {
    if (s_arc) {
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        lv_arc_set_value(s_arc, pct);
    }
    if (s_lbl_pct) {
        lv_label_set_text_fmt(s_lbl_pct, "%d%% used", pct);
    }
    if (s_lbl_reset) {
        if (reset_mins < 0) {
            lv_label_set_text(s_lbl_reset, "---");
        } else if (reset_mins < 60) {
            lv_label_set_text_fmt(s_lbl_reset, "Resets in %dm", reset_mins);
        } else {
            lv_label_set_text_fmt(s_lbl_reset, "Resets in %dh %dm",
                                  reset_mins / 60, reset_mins % 60);
        }
    }
}

lv_obj_t* codex_pet_get_root(void) {
    return s_container;
}

int codex_pet_idle_frame_count(void) { return CODEX_PET_IDLE_FRAMES; }
const lv_image_dsc_t* codex_pet_idle_frame_dsc(int idx) {
    if (idx < 0 || idx >= CODEX_PET_IDLE_FRAMES) return &s_idle_dscs[0];
    return &s_idle_dscs[idx];
}

void codex_pet_mini_init(lv_obj_t* parent, int x, int y, int box_px) {
    s_mini_img = lv_image_create(parent);
    lv_image_set_src(s_mini_img, &s_idle_dscs[0]);
    // Scale the 192×208 idle frame down to `box_px` tall, anchored top-left so
    // it sits exactly where the logo did. 256 = 100% in LVGL's image scale.
    int zoom = (box_px * 256) / CODEX_PET_IDLE_H;
    lv_image_set_pivot(s_mini_img, 0, 0);
    lv_image_set_scale(s_mini_img, zoom);
    lv_obj_set_pos(s_mini_img, x, y);
    // Don't intercept the screen-cycle click — let it bubble to the container.
    lv_obj_add_flag(s_mini_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_mini_img, LV_OBJ_FLAG_CLICKABLE);
}

void codex_pet_mini_set_visible(bool visible) {
    s_mini_visible = visible;
    if (visible) s_mini_last_ms = millis();
}
