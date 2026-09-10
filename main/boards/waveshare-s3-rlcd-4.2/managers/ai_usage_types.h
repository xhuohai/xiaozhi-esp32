#pragma once

#include <cstdint>
#include <ctime>

struct AiUsageWindow {
    char id[16] = {};
    int32_t window_seconds = 0;
    float used_percent = 0.0f;
    time_t reset_at = 0;
};

enum class UsageStatus {
    kUnavailable,
    kOk,
    kStale,
    kError,
};

struct GptUsage {
    UsageStatus status = UsageStatus::kUnavailable;

    char plan[16] = {};

    bool allowed = false;
    bool limit_reached = false;

    AiUsageWindow windows[4];
    uint8_t window_count = 0;

    float credits_balance = 0.0f;
    int reset_credits = 0;

    time_t last_success_at = 0;
};

struct CursorUsage {
    UsageStatus status = UsageStatus::kUnavailable;

    char plan[16] = {};

    float total_used_percent = 0.0f;
    float api_used_percent = 0.0f;

    bool grok_available = false;
    float grok_used_percent = 0.0f;
    time_t grok_reset_at = 0;

    bool on_demand_enabled = false;
    float on_demand_used = 0.0f;

    time_t cycle_start = 0;
    time_t cycle_end = 0;

    time_t last_success_at = 0;
};

struct AiUsageSnapshot {
    GptUsage gpt;
    CursorUsage cursor;

    time_t fetched_at = 0;
};
