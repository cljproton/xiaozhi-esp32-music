#include "handler_registry.h"
#include <esp_log.h>
#include <algorithm>

static const char* TAG = "HandlerRegistry";

void HandlerRegistry::Register(const httpd_uri_t& uri, const std::string& desc) {
    // 去重：同 uri+method 只保留第一个
    auto it = std::find_if(handlers_.begin(), handlers_.end(),
        [&uri](const Handler& h) {
            return h.uri.uri == uri.uri && h.uri.method == uri.method;
        });
    if (it == handlers_.end()) {
        handlers_.push_back({uri, desc});
        ESP_LOGD(TAG, "Registered: %s %s (%s)", 
                 uri.method == HTTP_GET ? "GET" : 
                 uri.method == HTTP_POST ? "POST" : "OTHER",
                 uri.uri, desc.c_str());
    } else {
        ESP_LOGW(TAG, "Duplicate handler skipped: %s %s",
                 uri.method == HTTP_GET ? "GET" : "POST", uri.uri);
    }
}

esp_err_t HandlerRegistry::RegisterAll(httpd_handle_t server) const {
    for (const auto& h : handlers_) {
        esp_err_t err = httpd_register_uri_handler(server, &h.uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", h.uri.uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}