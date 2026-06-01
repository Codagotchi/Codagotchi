#pragma once
#include <lvgl.h>

// Dedicated Codex pet screen. Owns one LVGL image that cycles through frames
// from codex_pet_frames.h. Switches between the `idle` and `running` row
// based on the Codex active flag (set by main.cpp from the daemon's "ac"
// field, ultimately sourced from chatgpt.com/backend-api/codex/tasks).
void codex_pet_init(lv_obj_t* parent);
void codex_pet_show(void);
void codex_pet_hide(void);
void codex_pet_tick(void);                 // called from main.cpp loop AFTER lv_timer_handler
void codex_pet_set_active(bool active);    // true → play "running", false → "idle"
// Updates the usage ring + labels shown around the idle pet. Ignored
// visually while the pet is active (ring/labels hide to give the running
// animation full focus). Values are the Codex 5-hour-window percent
// (0-100) and minutes until that window resets (-1 if unknown).
void codex_pet_set_usage(int pct, int reset_mins);
lv_obj_t* codex_pet_get_root(void);        // for hooking the click-to-cycle handler

// Expose idle frame descriptors so other screens can run their own Codex
// pet animation (e.g. the Bluetooth corner pet).
int                    codex_pet_idle_frame_count(void);
const lv_image_dsc_t*  codex_pet_idle_frame_dsc(int idx);

// Small idle-pet image for the corner of another screen (replaces the brand
// logo on the Codex usage screen). Scaled down from the idle spritesheet to
// fit `box_px` tall, anchored at (x, y) in `parent`. Animates only while
// visible — call codex_pet_mini_set_visible() when its host screen shows/hides.
void codex_pet_mini_init(lv_obj_t* parent, int x, int y, int box_px);
void codex_pet_mini_set_visible(bool visible);
