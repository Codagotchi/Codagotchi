#include "ui.h"
#include "splash.h"
#include "codex_pet.h"
#include "codex_pet_frames.h"
#include <lvgl.h>
#include "logo.h"
#include "icons.h"
#include "hal/board_caps.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_mono_32);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;

    // Timer screen
    const lv_font_t* timer_title_font;
    const lv_font_t* timer_countdown_font;
    int16_t timer_anim_size;
};
static Layout L = {};

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
        L.timer_title_font    = &font_tiempos_34;
        L.timer_countdown_font = &font_tiempos_56;
        L.timer_anim_size     = 120;
    } else {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
        L.timer_title_font    = &font_styrene_28;
        L.timer_countdown_font = &font_tiempos_34;
        L.timer_anim_size     = 90;
    }

    L.content_w = L.scr_w - 2 * L.margin;
}

// Design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets ----
// One widget set per provider — both screens share the same layout but
// hold independent values so a Codex payload doesn't clobber Claude's bars
// (and vice versa). Indexed by provider_t.
struct UsageScreenWidgets {
    lv_obj_t* container;
    lv_obj_t* lbl_title;
    lv_obj_t* bar_session;
    lv_obj_t* lbl_session_pct;
    lv_obj_t* lbl_session_label;
    lv_obj_t* lbl_session_reset;
    lv_obj_t* bar_weekly;
    lv_obj_t* lbl_weekly_pct;
    lv_obj_t* lbl_weekly_label;
    lv_obj_t* lbl_weekly_reset;
    lv_obj_t* lbl_anim;
    lv_obj_t* lbl_model;  // currently-selected model name, under the title
    bool active;  // host reports the provider's CLI is currently working
};
static UsageScreenWidgets usage_screens[PROVIDER_COUNT];

// ---- Model selection (cycled by the SECONDARY button) ----
// `label` is shown on screen; `cmd` is typed to the host as "/model <cmd>".
struct ModelDef { const char* label; const char* cmd; };
static const ModelDef claude_models[] = {
    {"Opus",   "opus"},
    {"Sonnet", "sonnet"},
    {"Haiku",  "haiku"},
};
static const ModelDef codex_models[] = {
    {"gpt-5.5",       "gpt-5.5"},
    {"gpt-5.4",       "gpt-5.4"},
    {"gpt-5.4-mini",  "gpt-5.4-mini"},
    {"gpt-5.3-codex", "gpt-5.3-codex"},
    {"gpt-5.2",       "gpt-5.2"},
};
static int model_idx[PROVIDER_COUNT] = {0, 0};

static const ModelDef* model_list(provider_t p, int* out_count) {
    if (p == PROVIDER_CODEX) {
        *out_count = (int)(sizeof(codex_models) / sizeof(codex_models[0]));
        return codex_models;
    }
    *out_count = (int)(sizeof(claude_models) / sizeof(claude_models[0]));
    return claude_models;
}

// ---- Timer screen widgets ----
static lv_obj_t* timer_container;
static lv_obj_t* timer_label;
static lv_obj_t* timer_mini_anim;
static lv_obj_t* timer_arc;
static int32_t   s_reset_secs  = -1;
static uint32_t  s_reset_sync_ms = 0;

// ---- Bluetooth screen widgets ----
static lv_obj_t* ble_container;
static lv_obj_t* lbl_ble_status;
static lv_obj_t* lbl_ble_device;
static lv_obj_t* lbl_ble_mac;

// ---- Logo (shared, on top) ----
static lv_obj_t* logo_img;

// ---- Claude usage screen corner pet ----
static lv_obj_t* claude_usage_pet_canvas;  // splash mini parented to usage container

// ---- Bluetooth screen pets (one per provider, only one shown at a time) ----
static lv_obj_t* ble_claude_pet;
static lv_obj_t* ble_codex_pet;
static lv_image_dsc_t ble_codex_pet_dsc;
static uint8_t  ble_codex_pet_frame = 0;
static uint32_t ble_codex_pet_last_ms = 0;

// ---- Claude pet screen (mirror of the Codex pet screen) ----
static lv_obj_t* claude_pet_container;
static lv_obj_t* claude_pet_mini;      // splash creature (Claude)
static lv_obj_t* claude_pet_arc;       // 5h-window usage ring
static lv_obj_t* claude_pet_lbl_pct;   // "X% used"
static lv_obj_t* claude_pet_lbl_reset; // "Resets in ..."

// ---- Feed screen (food-drop animation when middle button pressed) ----
static lv_obj_t* feed_container;
static lv_obj_t* feed_claude_pet;   // splash creature (Claude)
static lv_obj_t* feed_codex_pet;    // codex pet idle frame (Codex)
static lv_image_dsc_t feed_codex_dsc;
static lv_obj_t* feed_treat;        // the dropping food (placeholder: drawn circle)
static lv_obj_t* feed_lbl;          // status text
static uint8_t   feed_codex_frame = 0;
static uint32_t  feed_codex_last_ms = 0;
// The label shown while feeding — also the text typed to the host (the first
// request starts the 5-hour window).
static const char* FEED_PROMPT_CLAUDE = "Feeding Claude...";
static const char* FEED_PROMPT_CODEX  = "Feeding Codex...";

// ---- Model confirm screen (shows after cycling model) ----
static lv_obj_t* model_confirm_container;
static lv_obj_t* model_confirm_lbl_provider;
static lv_obj_t* model_confirm_lbl_model;
static lv_obj_t* model_confirm_claude_pet;   // splash creature (Claude)
static lv_obj_t* model_confirm_codex_pet;    // codex pet idle frame (Codex)
static lv_image_dsc_t model_confirm_codex_dsc;
static uint32_t model_confirm_show_time = 0;

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE_CLAUDE;
// Active UI mode: which provider's screens the PWR button cycles through.
// Toggled by the PRIMARY (left) button. Kept in sync whenever a provider-
// specific screen is shown (see ui_show_screen).
static provider_t active_mode = PROVIDER_CLAUDE;

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

// Map a usage screen to its provider, or PROVIDER_COUNT if `s` isn't one.
static provider_t screen_to_provider(screen_t s) {
    if (s == SCREEN_USAGE_CLAUDE) return PROVIDER_CLAUDE;
    if (s == SCREEN_USAGE_CODEX)  return PROVIDER_CODEX;
    return PROVIDER_COUNT;
}

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);
static void ble_reset_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, 16, 0);
    lv_obj_set_style_pad_right(panel, 16, 0);
    lv_obj_set_style_pad_top(panel, 12, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc(lv_image_dsc_t* dsc, int w, int h, const uint16_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.stride = w * 2;
    dsc->data = (const uint8_t*)data;
    dsc->data_size = w * h * 2;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &font_styrene_28, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, 18, 0);
    lv_obj_set_style_pad_right(lbl, 18, 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_bottom(lbl, 6, 0);
    return lbl;
}


// ======== Usage Screen ========

static void make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                             lv_obj_t** out_pct, lv_obj_t** out_pill,
                             lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, &font_styrene_48, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y, L.content_w - 32, 24);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, &font_styrene_28, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);
}

static void init_usage_screen(lv_obj_t* scr, provider_t provider, const char* title) {
    UsageScreenWidgets& w = usage_screens[provider];

    w.container = lv_obj_create(scr);
    lv_obj_set_size(w.container, L.scr_w, L.scr_h);
    lv_obj_set_pos(w.container, 0, 0);
    lv_obj_set_style_bg_opa(w.container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(w.container, 0, 0);
    lv_obj_set_style_pad_all(w.container, 0, 0);
    lv_obj_clear_flag(w.container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(w.container, global_click_cb, LV_EVENT_CLICKED, NULL);

    w.lbl_title = lv_label_create(w.container);
    lv_label_set_text(w.lbl_title, title);
    lv_obj_set_style_text_font(w.lbl_title, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(w.lbl_title, COL_TEXT, 0);
    lv_obj_align(w.lbl_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    // Selected-model label, centered just under the title, colored per the AI.
    int mcount;
    const ModelDef* mlist = model_list(provider, &mcount);
    w.lbl_model = lv_label_create(w.container);
    lv_label_set_text(w.lbl_model, mlist[model_idx[provider]].label);
    lv_obj_set_style_text_font(w.lbl_model, &font_styrene_20, 0);
    lv_color_t model_color = (provider == PROVIDER_CLAUDE)
        ? lv_color_hex(0xd97757)              // Orange (THEME_ACCENT)
        : lv_color_hex(CODEX_PET_COLOR_HEX);  // Teal/blue-green
    lv_obj_set_style_text_color(w.lbl_model, model_color, 0);
    lv_obj_align(w.lbl_model, LV_ALIGN_TOP_MID, 0, L.content_y - 28);

    make_usage_panel(w.container, L.content_y, "Current",
                     &w.lbl_session_pct, &w.lbl_session_label,
                     &w.bar_session, &w.lbl_session_reset);
    make_usage_panel(w.container,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &w.lbl_weekly_pct, &w.lbl_weekly_label,
                     &w.bar_weekly, &w.lbl_weekly_reset);

    w.lbl_anim = lv_label_create(w.container);
    lv_label_set_text(w.lbl_anim, "");
    lv_obj_set_style_text_font(w.lbl_anim, &font_mono_32, 0);
    lv_obj_set_style_text_color(w.lbl_anim, COL_ACCENT, 0);
    lv_obj_align(w.lbl_anim, LV_ALIGN_BOTTOM_MID, 0, -15);

    w.active = false;
}

// ======== Bluetooth Screen ========

static void init_bluetooth_screen(lv_obj_t* scr) {
    ble_container = lv_obj_create(scr);
    lv_obj_set_size(ble_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(ble_container, 0, 0);
    lv_obj_set_style_bg_opa(ble_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ble_container, 0, 0);
    lv_obj_set_style_pad_all(ble_container, 0, 0);
    lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ble_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl_ble_title = lv_label_create(ble_container);
    lv_label_set_text(lbl_ble_title, "Bluetooth");
    lv_obj_set_style_text_font(lbl_ble_title, L.bt_title_font, 0);
    lv_obj_set_style_text_color(lbl_ble_title, COL_TEXT, 0);
    lv_obj_align(lbl_ble_title, LV_ALIGN_TOP_MID, 16, L.title_y);

    lv_obj_t* p_info = make_panel(ble_container, L.margin, L.content_y,
                                  L.content_w, L.bt_info_panel_h);

    static lv_image_dsc_t icon_bt_dsc;
    init_icon_dsc(&icon_bt_dsc, ICON_BLUETOOTH_W, ICON_BLUETOOTH_H, icon_bluetooth_data);

    lv_obj_t* bt_img = lv_image_create(p_info);
    lv_image_set_src(bt_img, &icon_bt_dsc);
    lv_obj_set_pos(bt_img, 0, 0);

    lbl_ble_status = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_status, "Initializing...");
    lv_obj_set_style_text_font(lbl_ble_status, L.bt_status_font, 0);
    lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_status, 56, 2);

    lbl_ble_device = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_device, "Device: ---");
    lv_obj_set_style_text_font(lbl_ble_device, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_ble_device, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_device, 0, 64);

    lbl_ble_mac = lv_label_create(p_info);
    lv_label_set_text(lbl_ble_mac, "Address: ---");
    lv_obj_set_style_text_font(lbl_ble_mac, L.bt_device_font, 0);
    lv_obj_set_style_text_color(lbl_ble_mac, COL_DIM, 0);
    lv_obj_set_pos(lbl_ble_mac, 0, 100);

    int reset_y = L.content_y + L.bt_info_panel_h + 16;
    lv_obj_t* reset_zone = lv_obj_create(ble_container);
    lv_obj_set_pos(reset_zone, L.margin, reset_y);
    lv_obj_set_size(reset_zone, L.content_w, L.bt_reset_zone_h);
    lv_obj_set_style_bg_color(reset_zone, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(reset_zone, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(reset_zone, 8, 0);
    lv_obj_set_style_border_width(reset_zone, 0, 0);
    lv_obj_set_style_pad_column(reset_zone, 14, 0);
    lv_obj_set_flex_flow(reset_zone, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reset_zone, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(reset_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(reset_zone, ble_reset_click_cb, LV_EVENT_CLICKED, NULL);

    static lv_image_dsc_t icon_trash_dsc;
    init_icon_dsc(&icon_trash_dsc, ICON_TRASH2_W, ICON_TRASH2_H, icon_trash2_data);
    lv_obj_t* trash_img = lv_image_create(reset_zone);
    lv_image_set_src(trash_img, &icon_trash_dsc);

    lv_obj_t* reset_lbl = lv_label_create(reset_zone);
    lv_label_set_text(reset_lbl, "Reset Bluetooth");
    lv_obj_set_style_text_font(reset_lbl, L.bt_device_font, 0);
    lv_obj_set_style_text_color(reset_lbl, COL_DIM, 0);

    lv_obj_t* lbl_credit = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit, "by @rachelworld");
    lv_obj_set_style_text_font(lbl_credit, L.bt_credit_1_font, 0);
    lv_obj_set_style_text_color(lbl_credit, COL_DIM, 0);
    lv_obj_align(lbl_credit, LV_ALIGN_BOTTOM_MID, 0, -46);

    lv_obj_t* lbl_credit2 = lv_label_create(ble_container);
    lv_label_set_text(lbl_credit2, "Pixel art by @amaanbuilds");
    lv_obj_set_style_text_font(lbl_credit2, L.bt_credit_2_font, 0);
    lv_obj_set_style_text_color(lbl_credit2, COL_DIM, 0);
    lv_obj_align(lbl_credit2, LV_ALIGN_BOTTOM_MID, 0, -20);

    // Claude pet (splash creature) in the upper-left corner, same spot as the logo.
    ble_claude_pet = splash_mini_create(ble_container, LOGO_HEIGHT);
    if (ble_claude_pet)
        lv_obj_set_pos(ble_claude_pet, L.margin, L.title_y - 10);

    // Codex pet (scaled idle frame) in the same corner.
    ble_codex_pet_dsc.header.w      = CODEX_PET_IDLE_W;
    ble_codex_pet_dsc.header.h      = CODEX_PET_IDLE_H;
    ble_codex_pet_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    ble_codex_pet_dsc.header.stride = CODEX_PET_IDLE_W * 2;
    ble_codex_pet_dsc.data          = codex_pet_idle_frames[0];
    ble_codex_pet_dsc.data_size     = CODEX_PET_IDLE_W * CODEX_PET_IDLE_H * 2;
    ble_codex_pet = lv_image_create(ble_container);
    lv_image_set_src(ble_codex_pet, &ble_codex_pet_dsc);
    lv_image_set_pivot(ble_codex_pet, 0, 0);
    lv_image_set_scale(ble_codex_pet, (LOGO_HEIGHT * 256) / CODEX_PET_IDLE_H);
    lv_obj_set_pos(ble_codex_pet, L.margin, L.title_y - 10);

    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Timer Screen ========

static void init_timer_screen(lv_obj_t* scr) {
    timer_container = lv_obj_create(scr);
    lv_obj_set_size(timer_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(timer_container, 0, 0);
    lv_obj_set_style_bg_opa(timer_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(timer_container, 0, 0);
    lv_obj_set_style_pad_all(timer_container, 0, 0);
    lv_obj_clear_flag(timer_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(timer_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    // Arc ring — drawn first so it sits behind the content
    int arc_size  = (L.scr_w < L.scr_h ? L.scr_w : L.scr_h) - 2 * L.margin;
    int ring_w    = (L.scr_h >= 460) ? 22 : 18;

    timer_arc = lv_arc_create(timer_container);
    lv_obj_set_size(timer_arc, arc_size, arc_size);
    lv_obj_center(timer_arc);
    lv_arc_set_range(timer_arc, 0, 300);
    lv_arc_set_value(timer_arc, 0);
    lv_arc_set_bg_angles(timer_arc, 135, 45);   // 270° sweep, gap at the bottom
    lv_obj_set_style_arc_color(timer_arc, COL_PANEL, LV_PART_MAIN);
    lv_obj_set_style_arc_width(timer_arc, ring_w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(timer_arc, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(timer_arc, ring_w, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(timer_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(timer_arc, 0, 0);
    lv_obj_set_style_pad_all(timer_arc, 0, 0);
    // Hide the draggable knob
    lv_obj_set_style_bg_opa(timer_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_opa(timer_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(timer_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(timer_arc, LV_OBJ_FLAG_EVENT_BUBBLE);

    // Inner flex container: creature on top, label below, both centered
    int inner_w = arc_size - 2 * ring_w - 40;
    lv_obj_t* inner = lv_obj_create(timer_container);
    lv_obj_set_width(inner, inner_w);
    lv_obj_set_height(inner, LV_SIZE_CONTENT);
    lv_obj_center(inner);
    lv_obj_set_style_bg_opa(inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(inner, 0, 0);
    lv_obj_set_style_pad_all(inner, 0, 0);
    lv_obj_set_style_pad_row(inner, 8, 0);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(inner, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_flex_flow(inner, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(inner, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    timer_mini_anim = splash_mini_create(inner, L.timer_anim_size);

    timer_label = lv_label_create(inner);
    lv_label_set_text(timer_label, "--:--");
    lv_obj_set_style_text_font(timer_label, L.timer_countdown_font, 0);
    lv_obj_set_style_text_color(timer_label, COL_TEXT, 0);

    lv_obj_t* lbl_next_reset = lv_label_create(inner);
    lv_label_set_text(lbl_next_reset, "Next Reset");
    lv_obj_set_style_text_font(lbl_next_reset, L.timer_title_font, 0);
    lv_obj_set_style_text_color(lbl_next_reset, COL_DIM, 0);

    lv_obj_add_flag(timer_container, LV_OBJ_FLAG_HIDDEN);
}

static void init_model_confirm_screen(lv_obj_t* scr) {
    model_confirm_container = lv_obj_create(scr);
    lv_obj_set_size(model_confirm_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(model_confirm_container, 0, 0);
    lv_obj_set_style_bg_color(model_confirm_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(model_confirm_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(model_confirm_container, 0, 0);
    lv_obj_set_style_pad_all(model_confirm_container, 0, 0);
    lv_obj_clear_flag(model_confirm_container, LV_OBJ_FLAG_SCROLLABLE);

    // Provider name (e.g., "Claude" or "Codex"), centered — always white.
    model_confirm_lbl_provider = lv_label_create(model_confirm_container);
    lv_label_set_text(model_confirm_lbl_provider, "Claude");
    lv_obj_set_style_text_font(model_confirm_lbl_provider, &font_tiempos_56, 0);
    lv_obj_set_style_text_color(model_confirm_lbl_provider, COL_TEXT, 0);
    lv_obj_align(model_confirm_lbl_provider, LV_ALIGN_CENTER, 0, -50);

    // Model name, centered below provider — colored per the AI, smaller than provider.
    model_confirm_lbl_model = lv_label_create(model_confirm_container);
    lv_label_set_text(model_confirm_lbl_model, "Opus");
    lv_obj_set_style_text_font(model_confirm_lbl_model, &font_tiempos_34, 0);
    lv_obj_align(model_confirm_lbl_model, LV_ALIGN_CENTER, 0, 50);

    // Claude pet (splash creature) in the upper-left corner.
    model_confirm_claude_pet = splash_mini_create(model_confirm_container, LOGO_HEIGHT);
    if (model_confirm_claude_pet)
        lv_obj_set_pos(model_confirm_claude_pet, L.margin, L.title_y - 10);

    // Codex pet (scaled idle frame) in the same corner.
    model_confirm_codex_dsc.header.w     = CODEX_PET_IDLE_W;
    model_confirm_codex_dsc.header.h     = CODEX_PET_IDLE_H;
    model_confirm_codex_dsc.header.cf    = LV_COLOR_FORMAT_RGB565;
    model_confirm_codex_dsc.header.stride = CODEX_PET_IDLE_W * 2;
    model_confirm_codex_dsc.data         = codex_pet_idle_frames[0];
    model_confirm_codex_dsc.data_size    = CODEX_PET_IDLE_W * CODEX_PET_IDLE_H * 2;
    model_confirm_codex_pet = lv_image_create(model_confirm_container);
    lv_image_set_src(model_confirm_codex_pet, &model_confirm_codex_dsc);
    lv_image_set_pivot(model_confirm_codex_pet, 0, 0);
    lv_image_set_scale(model_confirm_codex_pet, (LOGO_HEIGHT * 256) / CODEX_PET_IDLE_H);
    lv_obj_set_pos(model_confirm_codex_pet, L.margin, L.title_y - 10);

    lv_obj_add_flag(model_confirm_container, LV_OBJ_FLAG_HIDDEN);
}

// Claude pet screen — mirrors the Codex pet screen: a centered creature with a
// usage ring around it, "X% used", and "Resets in ...". Orange-themed.
#define CLAUDE_PET_RING_DIAMETER 340
#define CLAUDE_PET_RING_WIDTH    14
#define CLAUDE_PET_RING_Y_OFFSET -40
#define CLAUDE_PET_LBL_PCT_Y     (CLAUDE_PET_RING_DIAMETER / 2 + CLAUDE_PET_RING_Y_OFFSET + 50)
#define CLAUDE_PET_LBL_RESET_Y   (CLAUDE_PET_RING_DIAMETER / 2 + CLAUDE_PET_RING_Y_OFFSET + 86)

static void init_claude_pet_screen(lv_obj_t* scr) {
    claude_pet_container = lv_obj_create(scr);
    lv_obj_set_size(claude_pet_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(claude_pet_container, 0, 0);
    lv_obj_set_style_bg_color(claude_pet_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(claude_pet_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(claude_pet_container, 0, 0);
    lv_obj_set_style_pad_all(claude_pet_container, 0, 0);
    lv_obj_clear_flag(claude_pet_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(claude_pet_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    // Ring behind the creature.
    claude_pet_arc = lv_arc_create(claude_pet_container);
    lv_obj_set_size(claude_pet_arc, CLAUDE_PET_RING_DIAMETER, CLAUDE_PET_RING_DIAMETER);
    lv_arc_set_range(claude_pet_arc, 0, 100);
    lv_arc_set_value(claude_pet_arc, 0);
    lv_arc_set_bg_angles(claude_pet_arc, 270, 270 - 1);  // full 360° track
    lv_arc_set_rotation(claude_pet_arc, 270);            // start at 12 o'clock
    lv_obj_set_style_arc_color(claude_pet_arc, lv_color_hex(0x202020), LV_PART_MAIN);
    lv_obj_set_style_arc_width(claude_pet_arc, CLAUDE_PET_RING_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_color(claude_pet_arc, COL_ACCENT, LV_PART_INDICATOR);  // orange
    lv_obj_set_style_arc_width(claude_pet_arc, CLAUDE_PET_RING_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(claude_pet_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(claude_pet_arc, 0, 0);
    lv_obj_set_style_pad_all(claude_pet_arc, 0, 0);
    lv_obj_set_style_bg_opa(claude_pet_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_border_opa(claude_pet_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(claude_pet_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(claude_pet_arc, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_align(claude_pet_arc, LV_ALIGN_CENTER, 0, CLAUDE_PET_RING_Y_OFFSET);

    // Claude creature (splash mini) centered inside the ring.
    claude_pet_mini = splash_mini_create(claude_pet_container, 180);
    if (claude_pet_mini) {
        lv_obj_align(claude_pet_mini, LV_ALIGN_CENTER, 0, CLAUDE_PET_RING_Y_OFFSET);
        lv_obj_add_flag(claude_pet_mini, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // "X% used" label below the ring.
    claude_pet_lbl_pct = lv_label_create(claude_pet_container);
    lv_label_set_text(claude_pet_lbl_pct, "--% used");
    lv_obj_set_style_text_font(claude_pet_lbl_pct, &font_styrene_28, 0);
    lv_obj_set_style_text_color(claude_pet_lbl_pct, COL_TEXT, 0);
    lv_obj_align(claude_pet_lbl_pct, LV_ALIGN_CENTER, 0, CLAUDE_PET_LBL_PCT_Y);

    // Reset countdown label below.
    claude_pet_lbl_reset = lv_label_create(claude_pet_container);
    lv_label_set_text(claude_pet_lbl_reset, "---");
    lv_obj_set_style_text_font(claude_pet_lbl_reset, &font_styrene_24, 0);
    lv_obj_set_style_text_color(claude_pet_lbl_reset, COL_TEXT, 0);
    lv_obj_align(claude_pet_lbl_reset, LV_ALIGN_CENTER, 0, CLAUDE_PET_LBL_RESET_Y);

    lv_obj_add_flag(claude_pet_container, LV_OBJ_FLAG_HIDDEN);
}

static void claude_pet_set_usage(int pct, int reset_mins) {
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    if (claude_pet_arc) lv_arc_set_value(claude_pet_arc, pct);
    if (claude_pet_lbl_pct) lv_label_set_text_fmt(claude_pet_lbl_pct, "%d%% used", pct);
    if (claude_pet_lbl_reset) {
        if (reset_mins < 0) {
            lv_label_set_text(claude_pet_lbl_reset, "---");
        } else if (reset_mins < 60) {
            lv_label_set_text_fmt(claude_pet_lbl_reset, "Resets in %dm", reset_mins);
        } else {
            lv_label_set_text_fmt(claude_pet_lbl_reset, "Resets in %dh %dm",
                                  reset_mins / 60, reset_mins % 60);
        }
    }
}

static void init_feed_screen(lv_obj_t* scr) {
    feed_container = lv_obj_create(scr);
    lv_obj_set_size(feed_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(feed_container, 0, 0);
    lv_obj_set_style_bg_color(feed_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(feed_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(feed_container, 0, 0);
    lv_obj_set_style_pad_all(feed_container, 0, 0);
    lv_obj_clear_flag(feed_container, LV_OBJ_FLAG_SCROLLABLE);

    // Claude creature, centered.
    feed_claude_pet = splash_mini_create(feed_container, 180);
    if (feed_claude_pet) lv_obj_align(feed_claude_pet, LV_ALIGN_CENTER, 0, -10);

    // Codex creature (scaled idle frame), centered.
    feed_codex_dsc.header.w      = CODEX_PET_IDLE_W;
    feed_codex_dsc.header.h      = CODEX_PET_IDLE_H;
    feed_codex_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    feed_codex_dsc.header.stride = CODEX_PET_IDLE_W * 2;
    feed_codex_dsc.data          = codex_pet_idle_frames[0];
    feed_codex_dsc.data_size     = CODEX_PET_IDLE_W * CODEX_PET_IDLE_H * 2;
    feed_codex_pet = lv_image_create(feed_container);
    lv_image_set_src(feed_codex_pet, &feed_codex_dsc);
    lv_image_set_scale(feed_codex_pet, (180 * 256) / CODEX_PET_IDLE_H);
    lv_obj_align(feed_codex_pet, LV_ALIGN_CENTER, 0, -10);

    // The treat — placeholder: a small drawn "cookie" circle. Swap for a
    // pixel-art food sprite later.
    feed_treat = lv_obj_create(feed_container);
    lv_obj_set_size(feed_treat, 30, 30);
    lv_obj_set_style_radius(feed_treat, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(feed_treat, lv_color_hex(0xC8923C), 0);  // cookie tan
    lv_obj_set_style_bg_opa(feed_treat, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(feed_treat, 0, 0);
    // A couple of darker "chips" for cookie flavor.
    lv_obj_t* chip1 = lv_obj_create(feed_treat);
    lv_obj_set_size(chip1, 6, 6);
    lv_obj_set_style_radius(chip1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(chip1, lv_color_hex(0x5a3a18), 0);
    lv_obj_set_style_border_width(chip1, 0, 0);
    lv_obj_align(chip1, LV_ALIGN_TOP_LEFT, 6, 8);
    lv_obj_t* chip2 = lv_obj_create(feed_treat);
    lv_obj_set_size(chip2, 6, 6);
    lv_obj_set_style_radius(chip2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(chip2, lv_color_hex(0x5a3a18), 0);
    lv_obj_set_style_border_width(chip2, 0, 0);
    lv_obj_align(chip2, LV_ALIGN_BOTTOM_RIGHT, -6, -7);

    // Status label below the pet.
    feed_lbl = lv_label_create(feed_container);
    lv_label_set_text(feed_lbl, "Feeding...");
    lv_obj_set_style_text_font(feed_lbl, &font_tiempos_34, 0);
    lv_obj_set_style_text_color(feed_lbl, COL_TEXT, 0);
    lv_obj_align(feed_lbl, LV_ALIGN_CENTER, 0, 110);

    lv_obj_add_flag(feed_container, LV_OBJ_FLAG_HIDDEN);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    init_icon_dsc_rgb565a8(&logo_dsc, LOGO_WIDTH, LOGO_HEIGHT, logo_data);

    init_usage_screen(scr, PROVIDER_CLAUDE, "Claude");
    init_usage_screen(scr, PROVIDER_CODEX,  "Codex");
    init_bluetooth_screen(scr);
    init_timer_screen(scr);
    init_model_confirm_screen(scr);
    init_claude_pet_screen(scr);
    init_feed_screen(scr);
    codex_pet_init(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }
    if (codex_pet_get_root()) {
        lv_obj_add_event_cb(codex_pet_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    logo_img = lv_image_create(scr);
    lv_image_set_src(logo_img, &logo_dsc);
    lv_obj_set_pos(logo_img, L.margin, L.title_y - 10);

    // Claude usage screen: splash creature in the corner instead of the logo.
    claude_usage_pet_canvas = splash_mini_create(
        usage_screens[PROVIDER_CLAUDE].container, LOGO_HEIGHT);
    if (claude_usage_pet_canvas) {
        lv_obj_set_pos(claude_usage_pet_canvas, L.margin, L.title_y - 10);
        lv_obj_add_flag(claude_usage_pet_canvas, LV_OBJ_FLAG_EVENT_BUBBLE);
    }

    // Codex usage screen: codex pet in the corner instead of the logo.
    codex_pet_mini_init(usage_screens[PROVIDER_CODEX].container,
                        L.margin, L.title_y - 10, LOGO_HEIGHT);

}

void ui_update(provider_t provider, const UsageData* data) {
    if (provider >= PROVIDER_COUNT) return;
    UsageScreenWidgets& w = usage_screens[provider];

    // Always update active animation — even heartbeat-only payloads (valid=false)
    // carry an "ac" field and should flip the spinner on/off immediately.
    w.active = data->active;
    if (provider == PROVIDER_CODEX) {
        // Drive the dedicated pet screen too — switches idle <-> running.
        codex_pet_set_active(data->active);
        if (data->valid) {
            codex_pet_set_usage((int)(data->session_pct + 0.5f),
                                data->session_reset_mins);
        }
    } else if (provider == PROVIDER_CLAUDE) {
        // Drive the Claude pet screen's ring + labels.
        if (data->valid) {
            claude_pet_set_usage((int)(data->session_pct + 0.5f),
                                 data->session_reset_mins);
        }
        // When active: hide ring + labels and center the pet (mirrors codex_pet_set_active).
        static bool prev_claude_active = false;
        if (data->active != prev_claude_active) {
            prev_claude_active = data->active;
            bool act = data->active;
            if (claude_pet_arc)
                act ? lv_obj_add_flag(claude_pet_arc, LV_OBJ_FLAG_HIDDEN)
                    : lv_obj_clear_flag(claude_pet_arc, LV_OBJ_FLAG_HIDDEN);
            if (claude_pet_lbl_pct)
                act ? lv_obj_add_flag(claude_pet_lbl_pct, LV_OBJ_FLAG_HIDDEN)
                    : lv_obj_clear_flag(claude_pet_lbl_pct, LV_OBJ_FLAG_HIDDEN);
            if (claude_pet_lbl_reset)
                act ? lv_obj_add_flag(claude_pet_lbl_reset, LV_OBJ_FLAG_HIDDEN)
                    : lv_obj_clear_flag(claude_pet_lbl_reset, LV_OBJ_FLAG_HIDDEN);
            if (claude_pet_mini) {
                // Canvas was created at 180 px (cell=9, GRID=20).
                static const int CANVAS = 180;
                if (act) {
                    int scale = ((L.scr_h - 60) * 256) / CANVAS;
                    lv_obj_set_style_transform_pivot_x(claude_pet_mini, CANVAS / 2, 0);
                    lv_obj_set_style_transform_pivot_y(claude_pet_mini, CANVAS / 2, 0);
                    lv_obj_set_style_transform_scale_x(claude_pet_mini, scale, 0);
                    lv_obj_set_style_transform_scale_y(claude_pet_mini, scale, 0);
                    lv_obj_align(claude_pet_mini, LV_ALIGN_CENTER, 0, 0);
                } else {
                    lv_obj_set_style_transform_scale_x(claude_pet_mini, 256, 0);
                    lv_obj_set_style_transform_scale_y(claude_pet_mini, 256, 0);
                    lv_obj_align(claude_pet_mini, LV_ALIGN_CENTER, 0, CLAUDE_PET_RING_Y_OFFSET);
                }
            }
        }
    }
    if (w.lbl_anim) {
        if (!w.active) {
            lv_label_set_text(w.lbl_anim, "Ready");
            lv_obj_set_style_text_color(w.lbl_anim, COL_GREEN, 0);
        } else {
            lv_obj_set_style_text_color(w.lbl_anim, COL_ACCENT, 0);
        }
    }

    if (!data->valid) return;

    int s_pct = (int)(data->session_pct + 0.5f);

    lv_label_set_text_fmt(w.lbl_session_pct, "%d%%", s_pct);
    lv_bar_set_value(w.bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(w.bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    char buf[48];
    format_reset_time(data->session_reset_mins, buf, sizeof(buf));
    lv_label_set_text(w.lbl_session_reset, buf);

    int w_pct = (int)(data->weekly_pct + 0.5f);
    lv_label_set_text_fmt(w.lbl_weekly_pct, "%d%%", w_pct);
    lv_bar_set_value(w.bar_weekly, w_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(w.bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);

    format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
    lv_label_set_text(w.lbl_weekly_reset, buf);
}

void ui_tick_anim(void) {
    // Animate the Claude pet creature while its screen is visible.
    if (current_screen == SCREEN_CLAUDE_PET && claude_pet_mini)
        splash_mini_tick(claude_pet_mini);
    if (current_screen == SCREEN_USAGE_CLAUDE && claude_usage_pet_canvas)
        splash_mini_tick(claude_usage_pet_canvas);

    // Animate BLE screen pets.
    if (current_screen == SCREEN_BLUETOOTH) {
        if (active_mode == PROVIDER_CLAUDE && ble_claude_pet)
            splash_mini_tick(ble_claude_pet);
        if (active_mode == PROVIDER_CODEX && ble_codex_pet) {
            uint32_t now = lv_tick_get();
            if (now - ble_codex_pet_last_ms >= 150) {
                ble_codex_pet_last_ms = now;
                ble_codex_pet_frame = (ble_codex_pet_frame + 1) % codex_pet_idle_frame_count();
                lv_image_set_src(ble_codex_pet, codex_pet_idle_frame_dsc(ble_codex_pet_frame));
            }
        }
    }

    provider_t p = screen_to_provider(current_screen);
    if (p == PROVIDER_COUNT) return;
    UsageScreenWidgets& w = usage_screens[p];
    if (!w.active) return;

    uint32_t now = lv_tick_get();

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms >= spinner_ms[anim_spinner_idx]) {
        anim_last_ms = now;
        anim_phase = (anim_phase + 1) % SPINNER_PHASES;
        anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                        : (SPINNER_PHASES - anim_phase);

        static char buf[80];
        snprintf(buf, sizeof(buf), "%s...",
                 anim_messages[anim_msg_idx]);
        lv_label_set_text(w.lbl_anim, buf);
    }
}

static screen_t prev_non_splash_screen = SCREEN_USAGE_CLAUDE;

static void global_click_cb(lv_event_t* e) {
    (void)e;
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_cycle_screen();
}

static void ble_reset_click_cb(lv_event_t* e) {
    (void)e;
    ble_clear_bonds();
}

void ui_show_screen(screen_t screen) {
    for (int i = 0; i < PROVIDER_COUNT; i++) {
        if (usage_screens[i].container)
            lv_obj_add_flag(usage_screens[i].container, LV_OBJ_FLAG_HIDDEN);
    }
    if (timer_container) lv_obj_add_flag(timer_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ble_container, LV_OBJ_FLAG_HIDDEN);
    if (model_confirm_container) lv_obj_add_flag(model_confirm_container, LV_OBJ_FLAG_HIDDEN);
    if (claude_pet_container) lv_obj_add_flag(claude_pet_container, LV_OBJ_FLAG_HIDDEN);
    if (feed_container) lv_obj_add_flag(feed_container, LV_OBJ_FLAG_HIDDEN);
    codex_pet_hide();
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:        splash_show(); break;
    case SCREEN_USAGE_CLAUDE:  lv_obj_clear_flag(usage_screens[PROVIDER_CLAUDE].container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_USAGE_CODEX:   lv_obj_clear_flag(usage_screens[PROVIDER_CODEX].container,  LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_CODEX_PET:     codex_pet_show(); break;
    case SCREEN_CLAUDE_PET:    if (claude_pet_container) lv_obj_clear_flag(claude_pet_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_TIMER:         if (timer_container) lv_obj_clear_flag(timer_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_BLUETOOTH:     lv_obj_clear_flag(ble_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_MODEL_CONFIRM: if (model_confirm_container) lv_obj_clear_flag(model_confirm_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_FEED:          if (feed_container) lv_obj_clear_flag(feed_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    if (logo_img) {
        // Screens with a pet in the corner replace the shared logo.
        // All screens now show a pet in the corner — logo is no longer shown anywhere.
        lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }
    codex_pet_mini_set_visible(screen == SCREEN_USAGE_CODEX);

    // BLE screen: show the pet matching the active mode.
    if (screen == SCREEN_BLUETOOTH) {
        bool is_codex = (active_mode == PROVIDER_CODEX);
        if (ble_claude_pet) {
            if (is_codex) lv_obj_add_flag(ble_claude_pet, LV_OBJ_FLAG_HIDDEN);
            else          lv_obj_clear_flag(ble_claude_pet, LV_OBJ_FLAG_HIDDEN);
        }
        if (ble_codex_pet) {
            if (is_codex) lv_obj_clear_flag(ble_codex_pet, LV_OBJ_FLAG_HIDDEN);
            else          lv_obj_add_flag(ble_codex_pet, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Keep the active mode coherent with the screen being shown. Provider-
    // specific screens pin the mode; shared screens (Timer/Bluetooth) and the
    // Splash leave it as whichever mode the user came from.
    if (screen == SCREEN_USAGE_CLAUDE || screen == SCREEN_CLAUDE_PET)    active_mode = PROVIDER_CLAUDE;
    else if (screen == SCREEN_USAGE_CODEX || screen == SCREEN_CODEX_PET) active_mode = PROVIDER_CODEX;

    if (screen != SCREEN_SPLASH) prev_non_splash_screen = screen;
    current_screen = screen;
}

// Cycle through the screens that belong to the active mode only.
//   Claude: Usage Claude -> Timer -> Bluetooth -> (loop)
//   Codex:  Usage Codex  -> Codex Pet -> Timer -> Bluetooth -> (loop)
// The Splash is not a rotation stop (it's the boot/idle screen — PWR cycles
// its animations there, handled in main.cpp).
void ui_cycle_screen(void) {
    screen_t next;
    if (active_mode == PROVIDER_CLAUDE) {
        switch (current_screen) {
        case SCREEN_USAGE_CLAUDE:  next = SCREEN_CLAUDE_PET;  break;
        case SCREEN_CLAUDE_PET:    next = SCREEN_BLUETOOTH;   break;
        case SCREEN_BLUETOOTH:     next = SCREEN_USAGE_CLAUDE; break;
        default:                   next = SCREEN_USAGE_CLAUDE; break;
        }
    } else {
        switch (current_screen) {
        case SCREEN_USAGE_CODEX:   next = SCREEN_CODEX_PET;   break;
        case SCREEN_CODEX_PET:     next = SCREEN_BLUETOOTH;   break;
        case SCREEN_BLUETOOTH:     next = SCREEN_USAGE_CODEX; break;
        default:                   next = SCREEN_USAGE_CODEX; break;
        }
    }
    ui_show_screen(next);
}

// Toggle Claude <-> Codex mode and jump to the new mode's primary usage screen.
void ui_toggle_mode(void) {
    active_mode = (active_mode == PROVIDER_CLAUDE) ? PROVIDER_CODEX : PROVIDER_CLAUDE;
    ui_show_screen(active_mode == PROVIDER_CLAUDE ? SCREEN_USAGE_CLAUDE : SCREEN_USAGE_CODEX);
}

provider_t ui_get_mode(void) {
    return active_mode;
}

void ui_show_model_confirm(provider_t p, const char* model_name) {
    if (!model_confirm_container) return;
    const char* provider_name = (p == PROVIDER_CLAUDE) ? "Claude" : "Codex";

    // Provider name stays white; model name carries the AI's color
    // (orange for Claude, teal for Codex).
    lv_label_set_text(model_confirm_lbl_provider, provider_name);
    lv_label_set_text(model_confirm_lbl_model, model_name);
    lv_color_t model_color = (p == PROVIDER_CLAUDE)
        ? lv_color_hex(0xd97757)              // Orange (THEME_ACCENT)
        : lv_color_hex(CODEX_PET_COLOR_HEX);  // Teal/blue-green
    lv_obj_set_style_text_color(model_confirm_lbl_model, model_color, 0);

    // Show the matching pet in the corner, hide the other.
    if (model_confirm_claude_pet) {
        if (p == PROVIDER_CLAUDE) lv_obj_clear_flag(model_confirm_claude_pet, LV_OBJ_FLAG_HIDDEN);
        else                      lv_obj_add_flag(model_confirm_claude_pet, LV_OBJ_FLAG_HIDDEN);
    }
    if (model_confirm_codex_pet) {
        if (p == PROVIDER_CODEX) lv_obj_clear_flag(model_confirm_codex_pet, LV_OBJ_FLAG_HIDDEN);
        else                     lv_obj_add_flag(model_confirm_codex_pet, LV_OBJ_FLAG_HIDDEN);
    }

    // Show the screen and force an immediate render so it appears before the
    // (blocking) HID keystrokes that follow in ui_cycle_model.
    ui_show_screen(SCREEN_MODEL_CONFIRM);
    model_confirm_show_time = millis();
    lv_refr_now(NULL);
}

// Auto-dismiss the model-confirm screen ~1s after it appears, transitioning to
// the active mode's usage screen. Called from the main loop.
void ui_tick_model_confirm(void) {
    if (current_screen != SCREEN_MODEL_CONFIRM) return;
    if (millis() - model_confirm_show_time >= 1000) {
        ui_show_screen(active_mode == PROVIDER_CLAUDE ? SCREEN_USAGE_CLAUDE : SCREEN_USAGE_CODEX);
    }
}

// Advance the active feed pet's animation by one (time-gated) frame.
static void feed_pet_tick(provider_t p) {
    if (p == PROVIDER_CLAUDE) {
        if (feed_claude_pet) splash_mini_tick(feed_claude_pet);
    } else if (feed_codex_pet) {
        uint32_t now = lv_tick_get();
        if (now - feed_codex_last_ms >= 150) {
            feed_codex_last_ms = now;
            feed_codex_frame = (feed_codex_frame + 1) % codex_pet_idle_frame_count();
            lv_image_set_src(feed_codex_pet, codex_pet_idle_frame_dsc(feed_codex_frame));
        }
    }
}

void ui_feed_pet(void) {
    if (!feed_container) return;
    provider_t p = active_mode;
    const char* msg = (p == PROVIDER_CLAUDE) ? FEED_PROMPT_CLAUDE : FEED_PROMPT_CODEX;

    // Show the pet matching the active mode.
    if (feed_claude_pet) {
        if (p == PROVIDER_CLAUDE) lv_obj_clear_flag(feed_claude_pet, LV_OBJ_FLAG_HIDDEN);
        else                      lv_obj_add_flag(feed_claude_pet, LV_OBJ_FLAG_HIDDEN);
    }
    if (feed_codex_pet) {
        if (p == PROVIDER_CODEX) lv_obj_clear_flag(feed_codex_pet, LV_OBJ_FLAG_HIDDEN);
        else                     lv_obj_add_flag(feed_codex_pet, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(feed_lbl, msg);
    lv_obj_clear_flag(feed_treat, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(feed_treat, LV_ALIGN_CENTER, 0, -300);  // start above the screen
    ui_show_screen(SCREEN_FEED);
    lv_refr_now(NULL);

    // Drop the treat onto the pet (blocking, rendered each step), animating
    // the pet as it falls.
    const int y_start = -300, y_end = -100, steps = 18;
    for (int i = 0; i <= steps; i++) {
        int y = y_start + (y_end - y_start) * i / steps;
        lv_obj_align(feed_treat, LV_ALIGN_CENTER, 0, y);
        feed_pet_tick(p);
        lv_refr_now(NULL);
        delay(25);
    }

    // Pet "eats" the treat.
    lv_obj_add_flag(feed_treat, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);

    // Type the message to the focused host CLI — this first request starts the
    // 5-hour usage window.
    char buf[40];
    snprintf(buf, sizeof(buf), "%s\n", msg);
    ble_keyboard_type(buf);

    // Animated hold, then back to the usage screen.
    uint32_t until = millis() + 1000;
    while ((int32_t)(millis() - until) < 0) {
        feed_pet_tick(p);
        lv_refr_now(NULL);
        delay(60);
    }
    ui_show_screen(p == PROVIDER_CLAUDE ? SCREEN_USAGE_CLAUDE : SCREEN_USAGE_CODEX);
}

void ui_cycle_model(void) {
    provider_t p = active_mode;
    int count;
    const ModelDef* list = model_list(p, &count);
    model_idx[p] = (model_idx[p] + 1) % count;
    const ModelDef* m = &list[model_idx[p]];
    if (usage_screens[p].lbl_model)
        lv_label_set_text(usage_screens[p].lbl_model, m->label);

    // Show the confirmation screen with the new model name.
    ui_show_model_confirm(p, m->label);

    // Tell the host CLI to switch model. Assumes the CLI is focused, same as
    // the other HID buttons.
    if (p == PROVIDER_CLAUDE) {
        // Claude Code accepts the model name as a direct argument.
        char buf[40];
        snprintf(buf, sizeof(buf), "/model %s\n", m->cmd);
        ble_keyboard_type(buf);
    } else {
        // Codex's /model is a standalone picker that opens highlighting the
        // current model. Open it, wait for it to render, then Down + Enter to
        // advance to the next entry — matching the label we just bumped.
        ble_keyboard_type("/model\n");
        delay(500);                   // let the model picker render
        ble_keyboard_press(0x51, 0);  // Down arrow → next model
        delay(30);                    // hold long enough to transmit before release
        ble_keyboard_release();
        delay(40);
        ble_keyboard_press(0x28, 0);  // Enter → confirm model
        delay(30);
        ble_keyboard_release();
        // Codex then shows a reasoning-effort picker with "medium" (default)
        // pre-selected. Accept it with Enter to complete the action.
        delay(400);                   // let the reasoning picker render
        ble_keyboard_press(0x28, 0);  // Enter → accept default (medium)
        delay(30);
        ble_keyboard_release();
    }
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    switch (state) {
    case BLE_STATE_CONNECTED:
        lv_label_set_text(lbl_ble_status, "Connected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_GREEN, 0);
        break;
    case BLE_STATE_ADVERTISING:
        lv_label_set_text(lbl_ble_status, "Advertising...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_AMBER, 0);
        break;
    case BLE_STATE_DISCONNECTED:
        lv_label_set_text(lbl_ble_status, "Disconnected");
        lv_obj_set_style_text_color(lbl_ble_status, COL_RED, 0);
        break;
    default:
        lv_label_set_text(lbl_ble_status, "Initializing...");
        lv_obj_set_style_text_color(lbl_ble_status, COL_DIM, 0);
        break;
    }

    if (name) {
        static char nbuf[48];
        snprintf(nbuf, sizeof(nbuf), "Device: %s", name);
        lv_label_set_text(lbl_ble_device, nbuf);
    }
    if (mac) {
        static char mbuf[48];
        snprintf(mbuf, sizeof(mbuf), "Address: %s", mac);
        lv_label_set_text(lbl_ble_mac, mbuf);
    }
}

void ui_update_timer(int reset_mins) {
    if (reset_mins < 0) { s_reset_secs = -1; return; }
    s_reset_secs    = (int32_t)reset_mins * 60;
    s_reset_sync_ms = lv_tick_get();
    ui_tick_timer();
}

void ui_tick_timer(void) {
    if (timer_mini_anim) splash_mini_tick(timer_mini_anim);

    if (!timer_label) return;
    if (s_reset_secs < 0) {
        lv_label_set_text(timer_label, "--:--");
        if (timer_arc) lv_arc_set_value(timer_arc, 0);
        return;
    }
    uint32_t elapsed_ms = lv_tick_get() - s_reset_sync_ms;
    int32_t remaining   = s_reset_secs - (int32_t)(elapsed_ms / 1000);
    if (remaining < 0) remaining = 0;
    int mm = remaining / 60;
    int ss = remaining % 60;
    static char buf[16];
    snprintf(buf, sizeof(buf), "%dm:%02ds", mm, ss);
    lv_label_set_text(timer_label, buf);
    if (timer_arc) lv_arc_set_value(timer_arc, 300 - (mm > 300 ? 300 : mm));
}

void ui_update_battery(int percent, bool charging) {
    (void)percent; (void)charging;  // Battery icon removed — USB-powered desk device.
}
