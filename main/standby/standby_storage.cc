#include "standby_storage.h"

#include "settings.h"

#include <esp_log.h>
#include <nvs.h>

#define TAG "StandbyStorage"

namespace {

constexpr const char* kNs = "standby";
constexpr const char* kKeyJpeg = "bg_jpeg";
constexpr const char* kKeyUrl = "bg_url";

// Max JPEG size we accept (16KB) - enforced by caller, but we double-check.
constexpr size_t kMaxJpegSize = 16384;

// NVS blob overhead guard: require free >= 2.5 * data_size
constexpr float kFreeFactor = 2.5f;

bool CheckNvsSpace(size_t need) {
    nvs_stats_t stats;
    // nvs_get_stats takes the partition name (default "nvs"), not a namespace.
    if (nvs_get_stats(NULL, &stats) != ESP_OK) {
        return false;
    }
    // Each entry slot occupies 32 bytes; free_entries includes reserved entries.
    size_t free_bytes = stats.free_entries * 32;
    return free_bytes >= static_cast<size_t>(need * kFreeFactor);
}

}  // namespace

namespace standby {

bool HasBackgroundImage() {
    Settings settings(kNs, false);
    return settings.GetString(kKeyJpeg, "").size() > 0;
}

bool LoadBackgroundImage(std::string& jpeg_data) {
    Settings settings(kNs, false);
    jpeg_data = settings.GetString(kKeyJpeg, "");
    return !jpeg_data.empty();
}

bool SaveBackgroundImage(const uint8_t* data, size_t len) {
    if (len == 0 || len > kMaxJpegSize) {
        ESP_LOGE(TAG, "Invalid JPEG size: %zu", len);
        return false;
    }
    if (!CheckNvsSpace(len)) {
        ESP_LOGE(TAG, "Not enough NVS space for image (%zu bytes needed, need %.1fx free)", len, kFreeFactor);
        return false;
    }
    Settings settings(kNs, true);
    std::string blob(reinterpret_cast<const char*>(data), len);
    settings.SetString(kKeyJpeg, blob);
    ESP_LOGI(TAG, "Saved standby background image (%zu bytes)", len);
    return true;
}

bool ClearBackgroundImage() {
    Settings settings(kNs, true);
    settings.EraseKey(kKeyJpeg);
    return true;
}

bool HasBackgroundUrl() {
    Settings settings(kNs, false);
    return settings.GetString(kKeyUrl, "").size() > 0;
}

std::string GetBackgroundUrl() {
    Settings settings(kNs, false);
    return settings.GetString(kKeyUrl, "");
}

bool SetBackgroundUrl(const std::string& url) {
    Settings settings(kNs, true);
    settings.SetString(kKeyUrl, url);
    return true;
}

bool ClearBackgroundUrl() {
    Settings settings(kNs, true);
    settings.EraseKey(kKeyUrl);
    return true;
}

size_t GetNvsFreeBytes() {
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) != ESP_OK) {
        return 0;
    }
    return stats.free_entries * 32;
}

}  // namespace standby