#include "ws_control_server.h"
#include "handlers/auth_api_handler.h"
#include "handlers/music_api_handler.h"
#include "handlers/system_api_handler.h"
#include "handlers/config_api_handler.h"
#include "application.h"
#if CONFIG_USE_RADIO
#include "radio/radio_api_handler.h"
#endif
#include <esp_log.h>
#include <esp_http_server.h>
#include <cstring>
#include <memory>
#include <vector>
#include <mutex>
#include <algorithm>

static const char* TAG = "WSControl";

WebSocketControlServer* WebSocketControlServer::instance_ = nullptr;

// 简单的客户端连接管理
static std::vector<int> s_client_fds;
static std::mutex s_client_mutex;

static void add_client(int fd) {
    std::lock_guard<std::mutex> lock(s_client_mutex);
    s_client_fds.push_back(fd);
    ESP_LOGD(TAG, "Client connected: fd=%d, total=%zu", fd, s_client_fds.size());
}

static void remove_client(int fd) {
    std::lock_guard<std::mutex> lock(s_client_mutex);
    auto it = std::find(s_client_fds.begin(), s_client_fds.end(), fd);
    if (it != s_client_fds.end()) {
        s_client_fds.erase(it);
        ESP_LOGD(TAG, "Client disconnected: fd=%d, total=%zu", fd, s_client_fds.size());
    }
}

static void broadcast_frame(const httpd_ws_frame_t* ws_pkt) {
    std::lock_guard<std::mutex> lock(s_client_mutex);
    auto* instance = WebSocketControlServer::GetInstance();
    if (!instance) return;
    httpd_handle_t handle = instance->GetServerHandle();
    for (int fd : s_client_fds) {
        httpd_ws_send_frame_async(handle, fd, const_cast<httpd_ws_frame_t*>(ws_pkt));
    }
}

WebSocketControlServer::WebSocketControlServer(const WebUIProvider& provider)
    : provider_(provider), renderer_(provider) {
    instance_ = this;
    
    // 注册所有 HTTP handlers
    AuthApiHandler::RegisterHandlers(registry_);
    MusicApiHandler::RegisterHandlers(registry_);
    SystemApiHandler::RegisterHandlers(registry_);
    config_api::RegisterHandlers(registry_);
#if CONFIG_USE_RADIO
    radio::RadioApiHandler::RegisterHandlers(registry_,
                                             Application::GetInstance().GetRadioSource());
#endif
    renderer_.RegisterRoutes(registry_);
}

WebSocketControlServer::~WebSocketControlServer() {
    Stop();
    instance_ = nullptr;
}

bool WebSocketControlServer::Start(int port) {
    config_ = HTTPD_DEFAULT_CONFIG();
    config_.server_port = port;
    config_.max_open_sockets = 7;
    config_.max_uri_handlers = CONFIG_HTTPD_MAX_URI_HANDLERS;
    config_.ctrl_port = 32769;

    // WebSocket URI
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = WsHandler,
        .user_ctx = nullptr,
        .is_websocket = true
    };

    if (httpd_start(&server_handle_, &config_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server on port %d", port);
        return false;
    }

    // 注册 WebSocket
    if (httpd_register_uri_handler(server_handle_, &ws_uri) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WebSocket handler");
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        return false;
    }

    // 注册所有 HTTP handlers
    if (registry_.RegisterAll(server_handle_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register HTTP handlers");
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "WebSocket + HTTP server started on port %d", port);
    return true;
}

void WebSocketControlServer::Stop() {
    if (server_handle_) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        ESP_LOGI(TAG, "Server stopped");
    }
}

esp_err_t WebSocketControlServer::WsHandler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        // WebSocket 握手 - 新连接建立
        add_client(httpd_req_to_sockfd(req));
        httpd_ws_frame_t ws_pkt;
        memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
        ws_pkt.type = HTTPD_WS_TYPE_TEXT;
        ws_pkt.payload = (uint8_t*)"connected";
        ws_pkt.len = strlen("connected");
        return httpd_ws_send_frame(req, &ws_pkt);
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // 接收消息
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (ws_pkt.len > 0) {
        std::unique_ptr<uint8_t[]> buf(new uint8_t[ws_pkt.len + 1]);
        ws_pkt.payload = buf.get();
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret == ESP_OK) {
            buf[ws_pkt.len] = '\0';
            const char* msg = reinterpret_cast<const char*>(buf.get());
            // 只记录 MCP 协议关键消息：tools/call, tools/result, initialize, ping/pong 过滤
            if (strstr(msg, "tools/call") || strstr(msg, "tools/result") || 
                strstr(msg, "tools/list") || strstr(msg, "initialize") ||
                strstr(msg, "sampling/") || strstr(msg, "notifications/")) {
                ESP_LOGI(TAG, "WS RX: %s", msg);
            }
            if (instance_ && instance_->ws_callback_) {
                instance_->ws_callback_(msg);
            }
        }
    } else {
        // 客户端发送空帧或关闭帧，移除连接
        remove_client(httpd_req_to_sockfd(req));
    }
    return ESP_OK;
}

esp_err_t WebSocketControlServer::Send(const std::string& payload) {
    if (!server_handle_) {
        return ESP_ERR_INVALID_STATE;
    }
    // 只记录 MCP 协议关键响应
    if (strstr(payload.c_str(), "tools/result") || strstr(payload.c_str(), "tools/list") ||
        strstr(payload.c_str(), "initialize") || strstr(payload.c_str(), "error")) {
        ESP_LOGI(TAG, "WS TX: %s", payload.c_str());
    }
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t*)payload.c_str();
    ws_pkt.len = payload.size();
    broadcast_frame(&ws_pkt);
    return ESP_OK;
}