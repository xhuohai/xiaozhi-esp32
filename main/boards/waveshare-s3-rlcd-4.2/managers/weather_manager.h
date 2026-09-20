#pragma once
#include <string>
#include "esp_http_client.h"

// 天气数据结构
struct WeatherData {
    std::string city;     // 城市名
    std::string temp;     // 温度（字符串）
    std::string text;     // 天气描述（如"晴"、"多云"）
    std::string update_time;
    bool valid = false;
};

// 天气管理器：国内 IP 库取城市和坐标 → 和风实时天气
// 不依赖 GeoAPI。和风 Geo 在部分 Key 的安全限制下会返回 403。
class WeatherManager {
public:
    static WeatherManager& getInstance();

    // 更新天气数据（包含定位+天气请求，耗时较长，应在后台任务中调用）
    bool update();

    WeatherData getLatestData() { return latest_data_; }

    void setApiConfig(const char* key, const char* host);

    // 可选：填写城市名则跳过 IP 定位；空或 "auto" 则按公网 IP 自动定位
    void setCity(const char* city);

    bool isConfigured() const;

    bool updateFromExternal(const std::string& city,
                            const std::string& weather_text,
                            const std::string& temperature,
                            const std::string& update_time = "");

private:
    WeatherManager();
    WeatherData latest_data_;

    std::string api_key_;
    std::string api_host_;
    std::string city_;

    // 上次成功的 IP 定位，失败时仍能继续拉天气，避免整段卡住后一直 -- --°C
    std::string cached_city_;
    double cached_lat_ = 0;
    double cached_lon_ = 0;
    bool cached_has_coord_ = false;
    bool has_cached_location_ = false;

    static esp_err_t http_event_handler(esp_http_client_event_t *evt);
    bool httpGet(const char* url, const char* host_header, int timeout_ms, bool request_gzip,
                 int* status_out, bool follow_redirect = true);
    const char* payloadJson();
    bool hasFixedCity() const;
    bool locateByIp(std::string* city, double* lat, double* lon, bool* has_coord);
    void parseWeatherJson(const char* json_data);
};
