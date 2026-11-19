#pragma once

#include <esp_http_server.h>
#include <string>
#include <vector>
#include <memory>

class HandlerRegistry {
public:
    struct Handler {
        httpd_uri_t uri;
        std::string description;
    };

    HandlerRegistry() = default;
    ~HandlerRegistry() = default;

    // 注册 handler（去重）
    void Register(const httpd_uri_t& uri, const std::string& desc = "");
    void Register(const Handler& h) { Register(h.uri, h.description); }

    // 批量注册到 httpd 实例
    esp_err_t RegisterAll(httpd_handle_t server) const;

    // 清空
    void Clear() { handlers_.clear(); }

    // 获取所有 handler（用于调试/日志）
    const std::vector<Handler>& GetAll() const { return handlers_; }

private:
    std::vector<Handler> handlers_;
};