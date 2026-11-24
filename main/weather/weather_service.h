#pragma once

#include <string>
#include <functional>

namespace weather {

struct WeatherInfo {
    float temperature = 0.0f;      // Celsius
    float temp_max = 0.0f;         // Daily high (Celsius)
    float temp_min = 0.0f;         // Daily low (Celsius)
    float humidity = 0.0f;         // Relative humidity (%)
    float wind_speed = 0.0f;       // km/h
    int weather_code = 0;          // WMO code
    std::string description;       // Localized: 晴/多云/小雨...
    int64_t updated_at = 0;        // Unix timestamp
    bool has_daily = false;        // temp_max/temp_min available
    bool has_details = false;      // humidity/wind_speed available
    bool valid = false;
};

// Localized description for a WMO weather code.
std::string Describe(int weather_code);

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