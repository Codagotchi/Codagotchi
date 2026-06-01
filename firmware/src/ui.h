#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE_CLAUDE,
    SCREEN_USAGE_CODEX,
    SCREEN_CODEX_PET,
    SCREEN_CLAUDE_PET,
    SCREEN_TIMER,
    SCREEN_BLUETOOTH,
    SCREEN_MODEL_CONFIRM,
    SCREEN_FEED,
    SCREEN_COUNT,
};

// Compatibility: existing code that references SCREEN_USAGE (the original
// single-provider screen) gets the Claude screen.
#define SCREEN_USAGE SCREEN_USAGE_CLAUDE

void ui_init(void);
void ui_update(provider_t provider, const UsageData* data);
void ui_update_timer(int reset_mins);
void ui_tick_anim(void);
void ui_tick_timer(void);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_mode(void);
provider_t ui_get_mode(void);
// Advance to the next model for the active mode's provider: updates the
// on-screen model label and types a "/model <name>" command to the host.
void ui_cycle_model(void);
// Show the model-confirm screen (provider + model, big and centered, pet in
// corner). Auto-dismisses via ui_tick_model_confirm().
void ui_show_model_confirm(provider_t p, const char* model_name);
void ui_tick_model_confirm(void);
// "Feed the pet": play the food-drop animation, type a warm-up prompt to the
// host (starting the 5h window), then return to the usage screen. Bound to the
// middle (PWR) button.
void ui_feed_pet(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
