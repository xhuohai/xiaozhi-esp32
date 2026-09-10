#pragma once

#include <atomic>
#include <cstdint>

#include <esp_bit_defs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "ai_usage_types.h"

class AiUsageManager {
public:
    static AiUsageManager& GetInstance();

    void Init();

    void RequestRefresh(bool force = false);

    AiUsageSnapshot GetSnapshot() const;

    bool IsRefreshing() const { return refreshing_.load(); }

    uint32_t Revision() const { return revision_.load(); }

    bool HasFreshCache(uint32_t max_age_s = 60) const;

    bool LastFetchOk() const { return last_fetch_ok_.load(); }

private:
    static constexpr EventBits_t kBitRefresh = BIT0;
    static constexpr EventBits_t kBitForce = BIT1;
    static constexpr uint32_t kAutoRefreshMs = 30 * 60 * 1000;
    static constexpr uint32_t kFreshCacheS = 60;

    AiUsageManager() = default;

    static void RefreshTaskEntry(void* arg);
    void RefreshTask();
    void MaybeFetch(bool force);
    bool FetchUsage(AiUsageSnapshot& snapshot, bool force);
    bool ParseUsageJson(const char* json, AiUsageSnapshot& snapshot);
    void StoreSnapshot(const AiUsageSnapshot& snapshot);
    void BumpRevision();

    mutable SemaphoreHandle_t mutex_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    TaskHandle_t task_ = nullptr;

    AiUsageSnapshot snapshot_{};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> refreshing_{false};
    std::atomic<bool> last_fetch_ok_{false};
    std::atomic<uint32_t> revision_{0};
    time_t last_attempt_at_ = 0;
};
