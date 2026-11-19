#include "system_api_handler.h"
#include "handler_registry.h"
#include "cjson_utils.h"
#include <esp_http_server.h>
#include <cJSON.h>
#include <string>

constexpr const char* kWebUiToken = "xiaozhi";

namespace {

esp_err_t SendJsonResponse(httpd_req_t* req, const char* status, const std::string& body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    httpd_resp_send(req, body.c_str(), body.size());
    return ESP_OK;
}

// Login handler
esp_err_t MusicLoginHandler(httpd_req_t* req) {
    constexpr size_t kMaxBodyLen = 256;
    if (req->content_len > kMaxBodyLen) {
        return ESP_FAIL;
    }

    char buf[kMaxBodyLen + 1];
    size_t received = 0;
    if (req->content_len > 0) {
        int ret = httpd_req_recv(req, buf, req->content_len);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received = static_cast<size_t>(ret);
    }
    buf[received] = '\0';

    CJsonUniquePtr root(cJSON_Parse(buf));
    std::string token;
    bool valid = false;
    if (root != nullptr) {
        auto token_item = cJSON_GetObjectItem(root.get(), "token");
        if (token_item != nullptr && cJSON_IsString(token_item)) {
            token = token_item->valuestring;
        }
        valid = (token == kWebUiToken);
    }

    if (!valid) {
        return SendJsonResponse(req, "401 Unauthorized", "{\"status\":\"error\",\"error\":\"invalid token\"}");
    }
    return SendJsonResponse(req, "200 OK", "{\"status\":\"ok\"}");
}

}  // namespace

void SystemApiHandler::RegisterHandlers(HandlerRegistry& registry) {
    // POST /api/music/login
    registry.Register({
        .uri = "/api/music/login",
        .method = HTTP_POST,
        .handler = MusicLoginHandler,
        .user_ctx = nullptr,
        .is_websocket = false
    }, "WebUI login with token");
}