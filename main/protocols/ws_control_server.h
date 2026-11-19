#pragma once

#include <esp_http_server.h>
#include <functional>
#include <string>
#include "web_ui/WebUIProvider.h"
#include "web_ui/WebUIRenderer.h"
#include "handlers/handler_registry.h"

class WebSocketControlServer {
public:
    explicit WebSocketControlServer(const WebUIProvider& provider);
    ~WebSocketControlServer();

    // 启动 WebSocket + HTTP 服务器
    bool Start(int port = 80);
    void Stop();

    // 发送 WebSocket 消息（用于 MCP 响应）
    esp_err_t Send(const std::string& payload);

    // WebSocket 消息回调
    using WsMessageCallback = std::function<void(const std::string&)>;
    void SetMessageCallback(WsMessageCallback cb) { ws_callback_ = std::move(cb); }

    // 获取服务器句柄（用于广播）
    httpd_handle_t GetServerHandle() const { return server_handle_; }
    static WebSocketControlServer* GetInstance() { return instance_; }

private:
    // WebSocket 协议处理
    static esp_err_t WsHandler(httpd_req_t* req);

    // HTTP 服务器
    httpd_handle_t server_handle_ = nullptr;
    httpd_config_t config_;
    
    const WebUIProvider& provider_;
    WebUIRenderer renderer_;
    HandlerRegistry registry_;
    
    WsMessageCallback ws_callback_;
    static WebSocketControlServer* instance_;
};