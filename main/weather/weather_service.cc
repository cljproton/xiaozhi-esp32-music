#include "weather_service.h"

#include "settings.h"
#include "application.h"

#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include <chrono>
#include <thread>

#define TAG "WeatherService"

namespace {

constexpr const char* kNs = "weather";
constexpr const char* kKeyCity = "city";
constexpr const char* kKeyLat = "lat";
constexpr const char* kKeyLon = "lon";
constexpr const char* kKeyCached = "cached";

// open-meteo endpoints
constexpr const char* kGeocodeUrl = "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=zh&format=json";
constexpr const char* kWeatherUrl = "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code&timezone=auto";
constexpr const char* kIpGeoUrl = "https://ipwho.is/?fields=latitude,longitude,city,country,timezone";

weather::WeatherInfo g_last_weather{};
std::string g_city;
float g_lat = 0.0f, g_lon = 0.0f;
using WeatherCallback = std::function<void(const weather::WeatherInfo&)>;
WeatherCallback g_callback;
bool g_running = false;
TaskHandle_t g_task_handle = nullptr;

// WMO weather code -> Chinese description
const char* WeatherCodeToDesc(int code) {
    switch (code) {
        case 0: return "晴";
        case 1: case 2: case 3: return "多云";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: return "毛毛雨";
        case 56: case 57: return "冻毛毛雨";
        case 61: case 63: case 65: return "雨";
        case 66: case 67: return "冻雨";
        case 71: case 73: case 75: return "雪";
        case 77: return "雪粒";
        case 80: case 81: case 82: return "阵雨";
        case 85: case 86: return "阵雪";
        case 95: return "雷暴";
        case 96: case 99: return "雷暴伴冰雹";
        default: return "未知";
    }
}

// Simple URL encode for city name
std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

// HTTP GET with cert bundle, returns body string (empty on failure)
std::string HttpGet(const char* url) {
    esp_http_client_config_t config = {};
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 10000;
    config.method = HTTP_METHOD_GET;
    config.buffer_size = 8192;
    config.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return "";

    std::string body;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        int64_t content_length = esp_http_client_fetch_headers(client);
        char buf[1024];
        int read;
        while ((read = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
            body.append(buf, read);
        }
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        return "";
    }
    return body;
}

// Parse geocoding result
bool ParseGeocode(const std::string& json, float& lat, float& lon) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;
    cJSON* results = cJSON_GetObjectItem(root, "results");
    if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        cJSON_Delete(root);
        return false;
    }
    cJSON* first = cJSON_GetArrayItem(results, 0);
    cJSON* lat_j = cJSON_GetObjectItem(first, "latitude");
    cJSON* lon_j = cJSON_GetObjectItem(first, "longitude");
    bool ok = false;
    if (cJSON_IsNumber(lat_j) && cJSON_IsNumber(lon_j)) {
        lat = static_cast<float>(lat_j->valuedouble);
        lon = static_cast<float>(lon_j->valuedouble);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

// Parse IP geolocation
bool ParseIpGeo(const std::string& json, float& lat, float& lon, std::string& city, std::string& tz) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;
    cJSON* lat_j = cJSON_GetObjectItem(root, "latitude");
    cJSON* lon_j = cJSON_GetObjectItem(root, "longitude");
    cJSON* city_j = cJSON_GetObjectItem(root, "city");
    cJSON* tz_j = cJSON_GetObjectItem(root, "timezone");
    bool ok = false;
    if (cJSON_IsNumber(lat_j) && cJSON_IsNumber(lon_j)) {
        lat = static_cast<float>(lat_j->valuedouble);
        lon = static_cast<float>(lon_j->valuedouble);
        if (cJSON_IsString(city_j)) city = city_j->valuestring;
        if (cJSON_IsString(tz_j)) tz = tz_j->valuestring;
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

// Parse current weather
bool ParseWeather(const std::string& json, weather::WeatherInfo& info) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return false;
    cJSON* current = cJSON_GetObjectItem(root, "current");
    if (!current) {
        cJSON_Delete(root);
        return false;
    }
    cJSON* temp = cJSON_GetObjectItem(current, "temperature_2m");
    cJSON* code = cJSON_GetObjectItem(current, "weather_code");
    cJSON* time = cJSON_GetObjectItem(current, "time");
    bool ok = false;
    if (cJSON_IsNumber(temp) && cJSON_IsNumber(code) && cJSON_IsString(time)) {
        info.temperature = static_cast<float>(temp->valuedouble);
        info.weather_code = code->valueint;
        info.description = WeatherCodeToDesc(info.weather_code);
        // time format: "2024-01-15T14:30" - parse to timestamp
        struct tm tm = {};
        strptime(time->valuestring, "%Y-%m-%dT%H:%M", &tm);
        info.updated_at = mktime(&tm);
        info.valid = true;
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

// Resolve coordinates from city or IP
bool ResolveCoordinates() {
    if (!g_city.empty()) {
        char url[512];
        snprintf(url, sizeof(url), kGeocodeUrl, UrlEncode(g_city).c_str());
        std::string resp = HttpGet(url);
        if (!resp.empty() && ParseGeocode(resp, g_lat, g_lon)) {
            ESP_LOGI(TAG, "Geocoded '%s' -> %.4f, %.4f", g_city.c_str(), g_lat, g_lon);
            return true;
        }
        ESP_LOGW(TAG, "Geocoding failed for '%s', falling back to IP", g_city.c_str());
    }
    // Fallback: IP geolocation
    std::string resp = HttpGet(kIpGeoUrl);
    std::string tz;
    if (!resp.empty() && ParseIpGeo(resp, g_lat, g_lon, g_city, tz)) {
        ESP_LOGI(TAG, "IP geolocation -> %.4f, %.4f (%s)", g_lat, g_lon, g_city.c_str());
        // 根据 IP 定位设置时区，供待机时间显示使用
        if (!tz.empty()) {
            setenv("TZ", tz.c_str(), 1);
            tzset();
        }
        return true;
    }
    ESP_LOGE(TAG, "Failed to resolve coordinates");
    return false;
}

// Fetch and cache weather
bool FetchWeather() {
    if (g_lat == 0.0f && g_lon == 0.0f) {
        if (!ResolveCoordinates()) return false;
    }
    char url[256];
    snprintf(url, sizeof(url), kWeatherUrl, g_lat, g_lon);
    std::string resp = HttpGet(url);
    if (resp.empty()) return false;

    weather::WeatherInfo info;
    if (!ParseWeather(resp, info)) return false;

    // Cache to NVS
    Settings settings(kNs, true);
    cJSON* json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "temp", info.temperature);
    cJSON_AddNumberToObject(json, "code", info.weather_code);
    cJSON_AddStringToObject(json, "desc", info.description.c_str());
    cJSON_AddNumberToObject(json, "time", info.updated_at);
    char* str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (str) {
        settings.SetString(kKeyCached, str);
        free(str);
    }
    g_last_weather = info;
    if (g_callback) g_callback(info);
    ESP_LOGI(TAG, "Weather updated: %.1f°C %s", info.temperature, info.description.c_str());
    return true;
}

// Load cached weather from NVS
void LoadCached() {
    Settings settings(kNs, false);
    std::string cached = settings.GetString(kKeyCached, "");
    if (cached.empty()) return;
    cJSON* json = cJSON_Parse(cached.c_str());
    if (!json) return;
    cJSON* temp = cJSON_GetObjectItem(json, "temp");
    cJSON* code = cJSON_GetObjectItem(json, "code");
    cJSON* desc = cJSON_GetObjectItem(json, "desc");
    cJSON* time = cJSON_GetObjectItem(json, "time");
    if (cJSON_IsNumber(temp) && cJSON_IsNumber(code) && cJSON_IsString(desc) && cJSON_IsNumber(time)) {
        g_last_weather.temperature = static_cast<float>(temp->valuedouble);
        g_last_weather.weather_code = code->valueint;
        g_last_weather.description = desc->valuestring;
        g_last_weather.updated_at = time->valueint;
        g_last_weather.valid = true;
    }
    cJSON_Delete(json);
}

// Background task
void WeatherTask(void* arg) {
    g_running = true;
    // Initial fetch
    FetchWeather();
    // Periodic: every 30 minutes
    while (g_running) {
        vTaskDelay(pdMS_TO_TICKS(30 * 60 * 1000));
        if (g_running) FetchWeather();
    }
    g_task_handle = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

namespace weather {

void Initialize(const std::string& city, std::function<void(const WeatherInfo&)> callback) {
    g_city = city;
    g_callback = callback;
    // Load persisted city
    Settings settings(kNs, false);
    if (g_city.empty()) {
        g_city = settings.GetString(kKeyCity, "");
    }
    if (g_city.empty()) {
        // Try load cached coords
        float lat = settings.GetInt(kKeyLat, 0) / 1000000.0f;
        float lon = settings.GetInt(kKeyLon, 0) / 1000000.0f;
        if (lat != 0.0f && lon != 0.0f) {
            g_lat = lat;
            g_lon = lon;
        }
    }
    LoadCached();
    // Start task if not already running
    if (!g_running) {
        xTaskCreatePinnedToCore(WeatherTask, "weather_task", 6144, nullptr, 3, &g_task_handle, 1);
    }
}

void SetCity(const std::string& city) {
    g_city = city;
    Settings settings(kNs, true);
    settings.SetString(kKeyCity, city);
    // Clear cached coords to force re-geocode
    settings.EraseKey(kKeyLat);
    settings.EraseKey(kKeyLon);
    // Trigger immediate update
    if (g_running) FetchWeather();
}

std::string GetCity() {
    return g_city;
}

void RequestUpdate() {
    if (g_running) FetchWeather();
}

WeatherInfo GetLastWeather() {
    return g_last_weather;
}

}  // namespace weather