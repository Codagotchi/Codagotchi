#pragma once
#include <Arduino.h>

enum provider_t {
    PROVIDER_CLAUDE = 0,
    PROVIDER_CODEX  = 1,
    PROVIDER_COUNT,
};

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
    bool active;             // true when the provider's CLI is actively working on host
};
