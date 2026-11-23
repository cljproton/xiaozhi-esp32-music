#pragma once

#include <string>
#include <functional>

namespace weather {

struct WeatherInfo {
    float temperature = 0.0f;      // Celsius
    int weather_code = 0;          // WMO code
    std::string description;       // Chinese: 晴/多云/小雨...
    int64_t updated_at = 0;        // Unix timestamp
    bool valid = false;
};

// Initialize weather service (starts background task).
// city: user-configured city name (empty = auto by IP).
// callback: invoked on each successful update.
void Initialize(const std::string& city, std::function<void(const WeatherInfo&)> callback);

// Set/get configured city (persisted in NVS).
void SetCity(const std::string& city);
std::string GetCity();

// Force an immediate update (async).
void RequestUpdate();

// Get last cached weather (valid=false if never fetched).
WeatherInfo GetLastWeather();

}  // namespace weather