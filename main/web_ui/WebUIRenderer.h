#pragma once

#include "WebUIProvider.h"
#include "handlers/handler_registry.h"
#include <esp_http_server.h>
#include <string>

class WebUIRenderer {
public:
    explicit WebUIRenderer(const WebUIProvider& provider);
    ~WebUIRenderer() = default;

    // 注册静态资源和页面路由
    void RegisterRoutes(HandlerRegistry& registry);

private:
    const WebUIProvider& provider_;
    std::string lang_;

    // 处理函数
    static esp_err_t HomeHandler(httpd_req_t* req);
    static esp_err_t IndexHandler(httpd_req_t* req);
    static esp_err_t BookHandler(httpd_req_t* req);
    
    // 生成 HTML（注入配置）
    std::string GenerateHomeHtml() const;
    std::string GenerateHtml() const;
};