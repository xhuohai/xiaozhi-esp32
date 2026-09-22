#pragma once

#include <atomic>
#include <string>

#include <esp_bit_defs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// 天气数据结构
struct WeatherData {
    std::string city;     // 城市名
    std::string temp;     // 温度（字符串）
    std::string text;     // 天气描述（如"晴"、"多云"）
    std::string update_time;
    bool valid = false;
};

// 天气管理器：国内 IP 库取城市名 → 补经纬度 → 和风实时天气
// HTTP 在独立任务里跑，不占用刷时钟的任务。
// 成功结果写入 NVS，重启后先显示上次天气。
class WeatherManager {
public:
    static WeatherManager& getInstance();

    void Init();

    // 非阻塞：排队一次后台刷新
    void RequestUpdate();

    bool IsUpdating() const { return pending_.load() || refreshing_.load(); }

    bool LastFetchOk() const { return last_fetch_ok_.load(); }

    WeatherData getLatestData();

    void setApiConfig(const char* key, const char* host);

    // 可选：填写城市名则跳过 IP 定位；空或 "auto" 则按公网 IP 自动定位
    void setCity(const char* city);

    bool isConfigured() const;

    bool updateFromExternal(const std::string& city,
                            const std::string& weather_text,
                            const std::string& temperature,
                            const std::string& update_time = "");

private:
    static constexpr EventBits_t kBitRefresh = BIT0;

    WeatherManager() = default;

    static void UpdateTaskEntry(void* arg);
    void UpdateTask();
    bool update();
    bool locateByIp(std::string* city, double* lat, double* lon, bool* has_coord);
    bool httpGet(const char* url, const char* host_header, int timeout_ms,
                 bool request_gzip, std::string* body, int* status_out);
    bool parseWeatherJson(const char* json_data);
    bool hasFixedCity() const;
    void LoadCache();
    void SaveCache();
    void StoreLatestLocked(const WeatherData& data);

    WeatherData latest_data_;
    std::string api_key_;
    std::string api_host_;
    std::string city_;

    std::string cached_city_;
    double cached_lat_ = 0;
    double cached_lon_ = 0;
    bool cached_has_coord_ = false;
    bool has_cached_location_ = false;

    SemaphoreHandle_t mutex_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> pending_{false};
    std::atomic<bool> refreshing_{false};
    std::atomic<bool> last_fetch_ok_{false};
};
