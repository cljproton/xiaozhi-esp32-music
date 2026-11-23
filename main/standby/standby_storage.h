#pragma once

#include <string>
#include <cstdint>

namespace standby {

// Load/save standby background image (baseline JPEG, <= 16KB, 292x240)
// Image is stored as raw JPEG bytes under NVS key "standby/bg_jpeg"
bool HasBackgroundImage();
bool LoadBackgroundImage(std::string& jpeg_data);
bool SaveBackgroundImage(const uint8_t* data, size_t len);
bool ClearBackgroundImage();

// URL fallback (stored as string under "standby/bg_url")
// Device downloads and caches to NVS on save/boot; keeps last cached JPEG.
bool HasBackgroundUrl();
std::string GetBackgroundUrl();
bool SetBackgroundUrl(const std::string& url);
bool ClearBackgroundUrl();

// NVS free space helper (returns bytes available)
size_t GetNvsFreeBytes();

}  // namespace standby