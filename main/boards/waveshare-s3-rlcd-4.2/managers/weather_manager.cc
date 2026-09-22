#include "weather_manager.h"

#include <algorithm>
#include <cctype>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <zlib.h>

#include "application.h"
#include "settings.h"

static const char *TAG = "WeatherManager";

static char* response_buffer = NULL;
static int response_len = 0;
static const int RESPONSE_BUFFER_SIZE = 8192;
static char* decompressed_buffer = NULL;
static const int DECOMPRESSED_BUFFER_SIZE = 8192;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA && response_buffer &&
        response_len + evt->data_len < RESPONSE_BUFFER_SIZE - 1) {
        memcpy(response_buffer + response_len, evt->data, evt->data_len);
        response_len += evt->data_len;
    }
    return ESP_OK;
}

static bool decompress_gzip_safe(const uint8_t* src, int src_len, char* dst, int dst_max_len, int* out_len) {
    if (src_len < 18 || src[0] != 0x1f || src[1] != 0x8b) return false;
    z_stream strm = {};
    strm.next_in = (Bytef*)src;
    strm.avail_in = src_len;
    strm.next_out = (Bytef*)dst;
    strm.avail_out = dst_max_len - 1;
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return false;
    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (ret != Z_STREAM_END && ret != Z_OK) return false;
    *out_len = dst_max_len - 1 - strm.avail_out;
    dst[*out_len] = '\0';
    return true;
}

WeatherManager& WeatherManager::getInstance() {
    static WeatherManager instance;
    return instance;
}

void WeatherManager::Init() {
    if (initialized_.exchange(true)) {
        return;
    }
    mutex_ = xSemaphoreCreateMutex();
    events_ = xEventGroupCreate();
    if (!mutex_ || !events_) {
        ESP_LOGE(TAG, "同步对象创建失败");
        return;
    }
    response_buffer = (char*)heap_caps_malloc(RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    decompressed_buffer = (char*)heap_caps_malloc(DECOMPRESSED_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    LoadCache();
    xTaskCreate(UpdateTaskEntry, "weather_fetch", 16 * 1024, this, 2, &task_);
}

void WeatherManager::RequestUpdate() {
    if (!events_) {
        return;
    }
    pending_.store(true);
    xEventGroupSetBits(events_, kBitRefresh);
}

WeatherData WeatherManager::getLatestData() {
    WeatherData copy;
    if (!mutex_) {
        return latest_data_;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    copy = latest_data_;
    xSemaphoreGive(mutex_);
    return copy;
}

void WeatherManager::setApiConfig(const char* key, const char* host) {
    api_key_ = key ? key : "";
    api_host_ = host ? host : "";
}

void WeatherManager::setCity(const char* city) {
    city_ = city ? city : "";
}

bool WeatherManager::isConfigured() const {
    if (api_key_.empty() || api_host_.empty()) {
        return false;
    }
    if (api_key_.find("your_") != std::string::npos ||
        api_host_.find("your_") != std::string::npos) {
        return false;
    }
    return true;
}

bool WeatherManager::hasFixedCity() const {
    if (city_.empty()) {
        return false;
    }
    std::string normalized = city_;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return normalized != "auto" && city_.find("your_") == std::string::npos;
}

void WeatherManager::StoreLatestLocked(const WeatherData& data) {
    latest_data_ = data;
}

bool WeatherManager::updateFromExternal(const std::string& city,
                                        const std::string& weather_text,
                                        const std::string& temperature,
                                        const std::string& update_time) {
    if (city.empty() || weather_text.empty() || temperature.empty()) {
        ESP_LOGW(TAG, "外部天气数据无效：city/text/temp 不能为空");
        return false;
    }

    WeatherData data;
    data.city = city;
    data.text = weather_text;
    data.temp = temperature;
    data.update_time = update_time.empty() ? "mcp" : update_time;
    data.valid = true;

    if (mutex_) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        StoreLatestLocked(data);
        xSemaphoreGive(mutex_);
    } else {
        latest_data_ = data;
    }
    cached_city_ = city;
    has_cached_location_ = true;
    SaveCache();

    ESP_LOGI(TAG, "天气已由外部写入: %s %s %s°C",
             data.city.c_str(), data.text.c_str(), data.temp.c_str());
    return true;
}

void WeatherManager::LoadCache() {
    Settings s("weather", false);
    std::string city = s.GetString("city", "");
    std::string text = s.GetString("text", "");
    std::string temp = s.GetString("temp", "");
    cached_city_ = city;
    cached_lat_ = s.GetInt("lat100", 0) / 100.0;
    cached_lon_ = s.GetInt("lon100", 0) / 100.0;
    cached_has_coord_ = s.GetBool("coord", false);
    has_cached_location_ = !cached_city_.empty();

    if (!city.empty() && !text.empty() && !temp.empty()) {
        WeatherData data;
        data.city = city;
        data.text = text;
        data.temp = temp;
        data.update_time = "nvs";
        data.valid = true;
        latest_data_ = data;
        ESP_LOGI(TAG, "已加载上次天气: %s %s %s°C", city.c_str(), text.c_str(), temp.c_str());
    }
}

void WeatherManager::SaveCache() {
    Settings s("weather", true);
    if (latest_data_.valid) {
        s.SetString("city", latest_data_.city);
        s.SetString("text", latest_data_.text);
        s.SetString("temp", latest_data_.temp);
    } else if (!cached_city_.empty()) {
        s.SetString("city", cached_city_);
    }
    s.SetInt("lat100", (int32_t)(cached_lat_ * 100));
    s.SetInt("lon100", (int32_t)(cached_lon_ * 100));
    s.SetBool("coord", cached_has_coord_);
}

void WeatherManager::UpdateTaskEntry(void* arg) {
    static_cast<WeatherManager*>(arg)->UpdateTask();
}

void WeatherManager::UpdateTask() {
    while (true) {
        xEventGroupWaitBits(events_, kBitRefresh, pdTRUE, pdFALSE, portMAX_DELAY);
        if (refreshing_.load()) {
            continue;
        }

        auto& app = Application::GetInstance();
        DeviceState ds = app.GetDeviceState();
        const bool in_audio_session = (ds == kDeviceStateConnecting ||
                                       ds == kDeviceStateListening ||
                                       ds == kDeviceStateSpeaking);
        if (in_audio_session) {
            ESP_LOGI(TAG, "对话中，天气刷新延后");
            vTaskDelay(pdMS_TO_TICKS(5000));
            RequestUpdate();
            continue;
        }

        refreshing_.store(true);
        bool ok = update();
        last_fetch_ok_.store(ok);
        if (ok) {
            SaveCache();
        }
        refreshing_.store(false);
        pending_.store(false);
    }
}

static std::string normalize_cn_city(std::string name) {
    const char* suffixes[] = {
        "特别行政区", "维吾尔自治区", "壮族自治区", "回族自治区", "自治区", "省", "市"
    };
    for (const char* suffix : suffixes) {
        const size_t suffix_len = strlen(suffix);
        if (name.size() > suffix_len &&
            name.compare(name.size() - suffix_len, suffix_len, suffix) == 0) {
            name.erase(name.size() - suffix_len);
            break;
        }
    }
    return name;
}

static bool parse_ip_location(const char* json, std::string* city, double* lat, double* lon, bool* has_coord) {
    cJSON* root = json ? cJSON_Parse(json) : nullptr;
    if (!root) {
        return false;
    }

    bool ok = false;
    *has_coord = false;
    cJSON* data = cJSON_GetObjectItem(root, "data");

    cJSON* location = data ? cJSON_GetObjectItem(data, "location") : nullptr;
    if (location && cJSON_IsArray(location) && cJSON_GetArraySize(location) >= 2) {
        cJSON* city_item = cJSON_GetArraySize(location) >= 3 ? cJSON_GetArrayItem(location, 2) : nullptr;
        cJSON* province_item = cJSON_GetArrayItem(location, 1);
        const char* raw = nullptr;
        if (city_item && cJSON_IsString(city_item) && city_item->valuestring[0] != '\0') {
            raw = city_item->valuestring;
        } else if (province_item && cJSON_IsString(province_item) && province_item->valuestring[0] != '\0') {
            raw = province_item->valuestring;
        }
        if (raw) {
            *city = normalize_cn_city(raw);
            ok = !city->empty();
        }
    }

    if (data) {
        cJSON* city_item = cJSON_GetObjectItem(data, "city");
        cJSON* prov_item = cJSON_GetObjectItem(data, "prov");
        if (!ok) {
            const char* raw = nullptr;
            if (city_item && cJSON_IsString(city_item) && city_item->valuestring[0] != '\0') {
                raw = city_item->valuestring;
            } else if (prov_item && cJSON_IsString(prov_item) && prov_item->valuestring[0] != '\0') {
                raw = prov_item->valuestring;
            }
            if (raw) {
                *city = normalize_cn_city(raw);
                ok = !city->empty();
            }
        }
        cJSON* lat_item = cJSON_GetObjectItem(data, "lat");
        cJSON* lon_item = cJSON_GetObjectItem(data, "lng");
        if (!lon_item) {
            lon_item = cJSON_GetObjectItem(data, "lon");
        }
        if (lat_item && lon_item && cJSON_IsString(lat_item) && cJSON_IsString(lon_item) &&
            lat_item->valuestring[0] != '\0' && lon_item->valuestring[0] != '\0') {
            *lat = atof(lat_item->valuestring);
            *lon = atof(lon_item->valuestring);
            *has_coord = (*lat != 0.0 || *lon != 0.0);
        } else if (lat_item && lon_item && cJSON_IsNumber(lat_item) && cJSON_IsNumber(lon_item)) {
            *lat = lat_item->valuedouble;
            *lon = lon_item->valuedouble;
            *has_coord = (*lat != 0.0 || *lon != 0.0);
        }
    }

    cJSON_Delete(root);
    return ok;
}

static bool lookup_cn_city_coord(const std::string& city, double* lat, double* lon) {
    static const struct {
        const char* name;
        double lat;
        double lon;
    } kCities[] = {
        {"北京", 39.90, 116.41}, {"上海", 31.23, 121.47}, {"天津", 39.13, 117.20},
        {"重庆", 29.56, 106.55}, {"哈尔滨", 45.80, 126.53}, {"长春", 43.88, 125.32},
        {"沈阳", 41.80, 123.43}, {"大连", 38.91, 121.61}, {"呼和浩特", 40.84, 111.75},
        {"石家庄", 38.04, 114.51}, {"太原", 37.87, 112.55}, {"济南", 36.67, 117.00},
        {"青岛", 36.07, 120.38}, {"郑州", 34.75, 113.63}, {"西安", 34.26, 108.94},
        {"兰州", 36.06, 103.83}, {"西宁", 36.62, 101.78}, {"银川", 38.49, 106.23},
        {"乌鲁木齐", 43.83, 87.62}, {"成都", 30.57, 104.07},
        {"贵阳", 26.65, 106.63}, {"昆明", 25.04, 102.71}, {"拉萨", 29.65, 91.11},
        {"武汉", 30.59, 114.31}, {"长沙", 28.23, 112.94}, {"南昌", 28.68, 115.86},
        {"合肥", 31.82, 117.23}, {"南京", 32.06, 118.80}, {"苏州", 31.30, 120.62},
        {"无锡", 31.49, 120.31}, {"杭州", 30.25, 120.16}, {"宁波", 29.87, 121.54},
        {"温州", 28.00, 120.70}, {"嘉兴", 30.75, 120.75}, {"绍兴", 30.00, 120.58},
        {"金华", 29.08, 119.65}, {"福州", 26.08, 119.30}, {"厦门", 24.48, 118.09},
        {"广州", 23.13, 113.26}, {"深圳", 22.54, 114.06}, {"东莞", 23.02, 113.75},
        {"佛山", 23.02, 113.12}, {"南宁", 22.82, 108.32}, {"海口", 20.05, 110.20},
        {"香港", 22.32, 114.17}, {"澳门", 22.19, 113.54}, {"台北", 25.03, 121.56},
    };
    for (const auto& item : kCities) {
        if (city == item.name) {
            *lat = item.lat;
            *lon = item.lon;
            return true;
        }
    }
    return false;
}

bool WeatherManager::httpGet(const char* url, const char* host_header, int timeout_ms,
                             bool request_gzip, std::string* body, int* status_out) {
    if (!response_buffer) {
        ESP_LOGW(TAG, "天气缓冲区未分配");
        return false;
    }

    response_len = 0;
    memset(response_buffer, 0, RESPONSE_BUFFER_SIZE);

    esp_http_client_config_t config = {};
    config.url = url;
    config.event_handler = http_event_handler;
    config.timeout_ms = timeout_ms;
    if (strncmp(url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }
    if (host_header && host_header[0] != '\0') {
        esp_http_client_set_header(client, "Host", host_header);
    }
    esp_http_client_set_header(client, "User-Agent", "ESP32-Weather-Station");
    if (request_gzip) {
        esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (status_out) {
        *status_out = status;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP GET 失败 err=%s status=%d url=%.80s",
                 esp_err_to_name(err), status, url);
        esp_http_client_cleanup(client);
        return false;
    }
    esp_http_client_cleanup(client);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP 状态=%d url=%.80s", status, url);
        return false;
    }

    int d_len = 0;
    if (decompressed_buffer &&
        decompress_gzip_safe((uint8_t*)response_buffer, response_len,
                             decompressed_buffer, DECOMPRESSED_BUFFER_SIZE, &d_len)) {
        *body = decompressed_buffer;
    } else if (response_len > 0) {
        response_buffer[response_len] = '\0';
        *body = response_buffer;
    } else {
        return false;
    }
    return true;
}

bool WeatherManager::locateByIp(std::string* city, double* lat, double* lon, bool* has_coord) {
    const char* url = "http://myip.ipip.net/json";
    std::string body;
    int status = 0;
    if (!httpGet(url, nullptr, 4000, false, &body, &status)) {
        ESP_LOGW(TAG, "IP 定位请求失败 status=%d", status);
        return false;
    }
    if (parse_ip_location(body.c_str(), city, lat, lon, has_coord)) {
        ESP_LOGI(TAG, "IP 定位成功: %s coord=%s (%.2f, %.2f)",
                 city->c_str(), *has_coord ? "yes" : "no", *lon, *lat);
        return true;
    }
    ESP_LOGW(TAG, "IP 定位响应无法解析 body=%.120s", body.c_str());
    return false;
}

bool WeatherManager::update() {
    if (!isConfigured()) {
        ESP_LOGW(TAG, "天气 API 未配置（请在 secret_config.h 填写和风 Key 与 Host）");
        return false;
    }

    std::string display_city;
    double lat = 0, lon = 0;
    bool has_coord = false;

    if (hasFixedCity()) {
        display_city = normalize_cn_city(city_);
        ESP_LOGI(TAG, "使用配置城市: %s", display_city.c_str());
    } else if (locateByIp(&display_city, &lat, &lon, &has_coord)) {
        cached_city_ = display_city;
        cached_lat_ = lat;
        cached_lon_ = lon;
        cached_has_coord_ = has_coord;
        has_cached_location_ = true;
    } else if (has_cached_location_) {
        display_city = cached_city_;
        lat = cached_lat_;
        lon = cached_lon_;
        has_coord = cached_has_coord_;
        ESP_LOGW(TAG, "公网 IP 定位失败，使用上次位置: %s", display_city.c_str());
    } else {
        ESP_LOGW(TAG, "公网 IP 定位失败");
        return false;
    }

    if (!has_coord && !display_city.empty()) {
        has_coord = lookup_cn_city_coord(display_city, &lat, &lon);
        if (has_coord) {
            ESP_LOGI(TAG, "城市坐标: %s (%.2f, %.2f)", display_city.c_str(), lon, lat);
            cached_city_ = display_city;
            cached_lat_ = lat;
            cached_lon_ = lon;
            cached_has_coord_ = true;
            has_cached_location_ = true;
        }
    }

    if (!has_coord) {
        ESP_LOGW(TAG, "没有可用的天气坐标 city=%s", display_city.c_str());
        return false;
    }

    char weather_url[512];
    snprintf(weather_url, sizeof(weather_url),
             "https://%s/v7/weather/now?location=%.2f%%2C%.2f&key=%s&lang=zh",
             api_host_.c_str(), lon, lat, api_key_.c_str());

    ESP_LOGI(TAG, "获取天气数据 city=%s coord=yes (%.2f, %.2f)",
             display_city.c_str(), lon, lat);
    std::string body;
    int status_code = 0;
    if (!httpGet(weather_url, api_host_.c_str(), 8000, true, &body, &status_code)) {
        ESP_LOGE(TAG, "天气请求失败 status=%d", status_code);
        return false;
    }
    if (body.empty()) {
        ESP_LOGE(TAG, "天气响应为空");
        return false;
    }

    if (!parseWeatherJson(body.c_str())) {
        return false;
    }
    if (!display_city.empty()) {
        if (mutex_) {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            latest_data_.city = display_city;
            xSemaphoreGive(mutex_);
        } else {
            latest_data_.city = display_city;
        }
    }
    return true;
}

bool WeatherManager::parseWeatherJson(const char* json_data) {
    cJSON *root = cJSON_Parse(json_data);
    if (!root) return false;
    bool ok = false;
    cJSON *code = cJSON_GetObjectItem(root, "code");
    const char* code_str = (code && cJSON_IsString(code)) ? code->valuestring : nullptr;
    if (code_str && strcmp(code_str, "200") == 0) {
        cJSON *now = cJSON_GetObjectItem(root, "now");
        cJSON *temp = now ? cJSON_GetObjectItem(now, "temp") : nullptr;
        cJSON *text = now ? cJSON_GetObjectItem(now, "text") : nullptr;
        if (temp && temp->valuestring && text && text->valuestring) {
            if (mutex_) {
                xSemaphoreTake(mutex_, portMAX_DELAY);
                latest_data_.temp = temp->valuestring;
                latest_data_.text = text->valuestring;
                latest_data_.valid = true;
                xSemaphoreGive(mutex_);
            } else {
                latest_data_.temp = temp->valuestring;
                latest_data_.text = text->valuestring;
                latest_data_.valid = true;
            }
            ESP_LOGI(TAG, "天气更新成功: %s°C, %s", temp->valuestring, text->valuestring);
            ok = true;
        }
    } else {
        ESP_LOGW(TAG, "天气响应异常 code=%s", code_str ? code_str : "(无)");
    }
    cJSON_Delete(root);
    return ok;
}
