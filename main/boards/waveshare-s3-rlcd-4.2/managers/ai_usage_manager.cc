#include "ai_usage_manager.h"

#include <cstring>
#include <ctime>
#include <string>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "application.h"
#include "board.h"
#include "../secret_config.h"

#ifndef AI_USAGE_API_URL
#define AI_USAGE_API_URL ""
#endif
#ifndef AI_USAGE_API_TOKEN
#define AI_USAGE_API_TOKEN ""
#endif

static const char* TAG = "AiUsage";

static bool CopyText(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0) {
        return false;
    }
    dst[0] = '\0';
    if (!src) {
        return false;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
    return dst[0] != '\0';
}

static UsageStatus ParseStatus(const char* text) {
    if (!text) {
        return UsageStatus::kUnavailable;
    }
    if (strcmp(text, "ok") == 0) {
        return UsageStatus::kOk;
    }
    if (strcmp(text, "stale") == 0) {
        return UsageStatus::kStale;
    }
    if (strcmp(text, "error") == 0) {
        return UsageStatus::kError;
    }
    return UsageStatus::kUnavailable;
}

static bool JsonGetString(cJSON* obj, const char* key, char* dst, size_t dst_size) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    return CopyText(dst, dst_size, item->valuestring);
}

static bool JsonGetNumber(cJSON* obj, const char* key, double* out) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

static bool JsonGetBool(cJSON* obj, const char* key, bool* out) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

static bool JsonGetTime(cJSON* obj, const char* key, time_t* out) {
    double value = 0;
    if (!JsonGetNumber(obj, key, &value)) {
        return false;
    }
    time_t epoch = static_cast<time_t>(value);
    if (epoch > static_cast<time_t>(10000000000LL)) {
        epoch /= 1000;
    }
    *out = epoch;
    return true;
}

static bool UrlLooksConfigured(const char* url) {
    if (!url || url[0] == '\0') {
        return false;
    }
    return strstr(url, "192.168.x.x") == nullptr;
}

AiUsageManager& AiUsageManager::GetInstance() {
    static AiUsageManager instance;
    return instance;
}

void AiUsageManager::Init() {
    if (initialized_.exchange(true)) {
        return;
    }
    mutex_ = xSemaphoreCreateMutex();
    events_ = xEventGroupCreate();
    if (!mutex_ || !events_) {
        ESP_LOGE(TAG, "failed to create sync objects");
        return;
    }
    xTaskCreate(RefreshTaskEntry, "ai_usage", 8192, this, 2, &task_);
}

void AiUsageManager::RequestRefresh(bool force) {
    if (!events_) {
        return;
    }
    xEventGroupSetBits(events_, force ? (kBitRefresh | kBitForce) : kBitRefresh);
}

AiUsageSnapshot AiUsageManager::GetSnapshot() const {
    AiUsageSnapshot copy;
    if (!mutex_) {
        return copy;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    copy = snapshot_;
    xSemaphoreGive(mutex_);
    return copy;
}

bool AiUsageManager::HasFreshCache(uint32_t max_age_s) const {
    time_t fetched_at = 0;
    if (mutex_) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        fetched_at = snapshot_.fetched_at;
        xSemaphoreGive(mutex_);
    }
    if (fetched_at <= 0) {
        return false;
    }
    time_t now = 0;
    time(&now);
    return now >= fetched_at && static_cast<uint32_t>(now - fetched_at) <= max_age_s;
}

void AiUsageManager::RefreshTaskEntry(void* arg) {
    static_cast<AiUsageManager*>(arg)->RefreshTask();
}

void AiUsageManager::RefreshTask() {
    vTaskDelay(pdMS_TO_TICKS(8000));
    RequestRefresh(false);

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            events_, kBitRefresh | kBitForce, pdTRUE, pdFALSE, pdMS_TO_TICKS(kAutoRefreshMs));
        bool force = (bits & kBitForce) != 0;
        bool requested = (bits & (kBitRefresh | kBitForce)) != 0;
        if (!requested) {
            force = false;
        }
        MaybeFetch(force);
    }
}

void AiUsageManager::MaybeFetch(bool force) {
    if (refreshing_.load()) {
        return;
    }
    if (!force && HasFreshCache(kFreshCacheS)) {
        return;
    }

    auto& app = Application::GetInstance();
    DeviceState ds = app.GetDeviceState();
    const bool in_audio_session = (ds == kDeviceStateConnecting ||
                                   ds == kDeviceStateListening ||
                                   ds == kDeviceStateSpeaking);
    if (in_audio_session) {
        ESP_LOGI(TAG, "defer refresh, audio session active");
        vTaskDelay(pdMS_TO_TICKS(5000));
        RequestRefresh(force);
        return;
    }

    refreshing_.store(true);
    BumpRevision();

    AiUsageSnapshot parsed;
    bool ok = FetchUsage(parsed, force);
    last_fetch_ok_.store(ok);
    if (ok) {
        StoreSnapshot(parsed);
        ESP_LOGI(TAG, "parsed gpt=%d cursor=%d gpt_plan=%s cursor_plan=%s heap=%u psram=%u",
                 static_cast<int>(parsed.gpt.status),
                 static_cast<int>(parsed.cursor.status),
                 parsed.gpt.plan,
                 parsed.cursor.plan,
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    } else {
        ESP_LOGW(TAG, "fetch failed, keep last-known-good heap=%u psram=%u",
                 static_cast<unsigned>(esp_get_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }

    time(&last_attempt_at_);
    refreshing_.store(false);
    BumpRevision();
}

bool AiUsageManager::FetchUsage(AiUsageSnapshot& snapshot, bool force) {
    if (!UrlLooksConfigured(AI_USAGE_API_URL)) {
        ESP_LOGW(TAG, "API URL not configured");
        return false;
    }

    auto* network = Board::GetInstance().GetNetwork();
    if (!network) {
        ESP_LOGW(TAG, "network not ready");
        return false;
    }

    auto http = network->CreateHttp(5);
    if (!http) {
        ESP_LOGW(TAG, "failed to create HTTP client");
        return false;
    }

    http->SetTimeout(15000);

    std::string auth = "Bearer ";
    auth += AI_USAGE_API_TOKEN;
    http->SetHeader("Authorization", auth.c_str());
    http->SetHeader("Accept", "application/json");
    if (force) {
        http->SetHeader("Cache-Control", "no-cache");
        http->SetHeader("X-Refresh", "1");
    }

    if (!http->Open("GET", AI_USAGE_API_URL)) {
        ESP_LOGW(TAG, "HTTP open failed");
        return false;
    }

    int status = http->GetStatusCode();
    std::string body = http->ReadAll();
    http->Close();

    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status=%d", status);
        return false;
    }
    if (body.empty()) {
        ESP_LOGW(TAG, "empty body");
        return false;
    }

    return ParseUsageJson(body.c_str(), snapshot);
}

static void ParseGpt(cJSON* gpt, GptUsage& out) {
    char status[16] = {};
    if (JsonGetString(gpt, "status", status, sizeof(status))) {
        out.status = ParseStatus(status);
    } else {
        out.status = UsageStatus::kError;
    }
    JsonGetString(gpt, "plan", out.plan, sizeof(out.plan));
    JsonGetBool(gpt, "allowed", &out.allowed);
    JsonGetBool(gpt, "limit_reached", &out.limit_reached);
    JsonGetTime(gpt, "last_success_at", &out.last_success_at);

    double credits = 0;
    if (JsonGetNumber(gpt, "credits_balance", &credits)) {
        out.credits_balance = static_cast<float>(credits);
    }
    double reset_credits = 0;
    if (JsonGetNumber(gpt, "reset_credits", &reset_credits)) {
        out.reset_credits = static_cast<int>(reset_credits);
    }

    cJSON* windows = cJSON_GetObjectItem(gpt, "windows");
    if (!cJSON_IsArray(windows)) {
        return;
    }
    int count = cJSON_GetArraySize(windows);
    if (count > 4) {
        count = 4;
    }
    out.window_count = 0;
    for (int i = 0; i < count; ++i) {
        cJSON* item = cJSON_GetArrayItem(windows, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }
        AiUsageWindow& window = out.windows[out.window_count];
        JsonGetString(item, "id", window.id, sizeof(window.id));
        double seconds = 0;
        if (JsonGetNumber(item, "window_seconds", &seconds)) {
            window.window_seconds = static_cast<int32_t>(seconds);
        }
        double used = 0;
        if (JsonGetNumber(item, "used_percent", &used)) {
            window.used_percent = static_cast<float>(used);
        }
        JsonGetTime(item, "reset_at", &window.reset_at);
        out.window_count++;
    }
}

static void ParseCursor(cJSON* cursor, CursorUsage& out) {
    char status[16] = {};
    if (JsonGetString(cursor, "status", status, sizeof(status))) {
        out.status = ParseStatus(status);
    } else {
        out.status = UsageStatus::kError;
    }
    JsonGetString(cursor, "plan", out.plan, sizeof(out.plan));
    JsonGetTime(cursor, "last_success_at", &out.last_success_at);
    JsonGetTime(cursor, "cycle_start", &out.cycle_start);
    JsonGetTime(cursor, "cycle_end", &out.cycle_end);

    double total_used = 0;
    if (JsonGetNumber(cursor, "total_used_percent", &total_used)) {
        out.total_used_percent = static_cast<float>(total_used);
    }
    double api_used = 0;
    if (JsonGetNumber(cursor, "api_used_percent", &api_used)) {
        out.api_used_percent = static_cast<float>(api_used);
    }

    cJSON* grok = cJSON_GetObjectItem(cursor, "grok_bot");
    if (cJSON_IsObject(grok)) {
        JsonGetBool(grok, "available", &out.grok_available);
        double grok_used = 0;
        if (JsonGetNumber(grok, "used_percent", &grok_used)) {
            out.grok_used_percent = static_cast<float>(grok_used);
        }
        JsonGetTime(grok, "reset_at", &out.grok_reset_at);
    }

    cJSON* on_demand = cJSON_GetObjectItem(cursor, "on_demand");
    if (cJSON_IsObject(on_demand)) {
        JsonGetBool(on_demand, "enabled", &out.on_demand_enabled);
        double used = 0;
        if (JsonGetNumber(on_demand, "used", &used)) {
            out.on_demand_used = static_cast<float>(used);
        }
    }
}

bool AiUsageManager::ParseUsageJson(const char* json, AiUsageSnapshot& snapshot) {
    cJSON* root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return false;
    }

    double schema = 0;
    if (!JsonGetNumber(root, "schema_version", &schema) || static_cast<int>(schema) != 1) {
        ESP_LOGW(TAG, "unsupported schema_version");
        cJSON_Delete(root);
        return false;
    }

    JsonGetTime(root, "generated_at", &snapshot.fetched_at);
    if (snapshot.fetched_at <= 0) {
        time(&snapshot.fetched_at);
    }

    cJSON* gpt = cJSON_GetObjectItem(root, "gpt");
    if (cJSON_IsObject(gpt)) {
        ParseGpt(gpt, snapshot.gpt);
    } else {
        snapshot.gpt.status = UsageStatus::kError;
    }

    cJSON* cursor = cJSON_GetObjectItem(root, "cursor");
    if (cJSON_IsObject(cursor)) {
        ParseCursor(cursor, snapshot.cursor);
    } else {
        snapshot.cursor.status = UsageStatus::kError;
    }

    cJSON* gpt_error = cJSON_IsObject(gpt) ? cJSON_GetObjectItem(gpt, "error_code") : nullptr;
    cJSON* cursor_error = cJSON_IsObject(cursor) ? cJSON_GetObjectItem(cursor, "error_code") : nullptr;
    ESP_LOGI(TAG, "schema=1 gpt=%s cursor=%s gpt_err=%s cursor_err=%s",
             snapshot.gpt.plan[0] ? snapshot.gpt.plan : "-",
             snapshot.cursor.plan[0] ? snapshot.cursor.plan : "-",
             (cJSON_IsString(gpt_error) && gpt_error->valuestring) ? gpt_error->valuestring : "-",
             (cJSON_IsString(cursor_error) && cursor_error->valuestring) ? cursor_error->valuestring : "-");

    cJSON_Delete(root);
    return true;
}

void AiUsageManager::StoreSnapshot(const AiUsageSnapshot& snapshot) {
    if (!mutex_) {
        return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    snapshot_ = snapshot;
    xSemaphoreGive(mutex_);
}

void AiUsageManager::BumpRevision() {
    revision_.fetch_add(1);
}
