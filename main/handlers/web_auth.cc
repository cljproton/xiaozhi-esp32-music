#include "web_auth.h"

#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <esp_log.h>

namespace web_auth {

namespace {

constexpr const char* kNamespace = "web_auth";
constexpr const char* kKey = "password";
constexpr const char* TAG = "WebAuth";

std::string cached_password_;
std::mutex password_mutex_;

char HexDigit(unsigned char v) { return v < 10 ? '0' + v : 'a' + (v - 10); }

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string GetPassword() {
    std::lock_guard<std::mutex> lock(password_mutex_);
    if (!cached_password_.empty()) {
        return cached_password_;
    }
    Settings settings(kNamespace, false);
    cached_password_ = settings.GetString(kKey, kDefaultPassword);
    if (cached_password_.empty()) {
        cached_password_ = kDefaultPassword;
    }
    return cached_password_;
}

bool IsDefaultPassword() { return GetPassword() == kDefaultPassword; }

bool CheckPassword(const std::string& password) { return password == GetPassword(); }

bool ChangePassword(const std::string& old_password, const std::string& new_password) {
    std::lock_guard<std::mutex> lock(password_mutex_);
    if (new_password.empty() || new_password.size() > 64) {
        return false;
    }
    // 检查和写入必须原子，否则两个并发请求都会以为旧密码正确。
    std::string current = cached_password_;
    if (current.empty()) {
        Settings settings(kNamespace, false);
        current = settings.GetString(kKey, kDefaultPassword);
        if (current.empty()) current = kDefaultPassword;
    }
    if (old_password != current || new_password == current) {
        return false;
    }
    Settings settings(kNamespace, true);
    settings.SetString(kKey, new_password);
    cached_password_ = new_password;
    ESP_LOGI(TAG, "Web UI password changed");
    return true;
}

std::string EncodeCookieValue(const std::string& password) {
    std::string out;
    out.reserve(password.size() * 2);
    for (unsigned char c : password) {
        out.push_back(HexDigit(c >> 4));
        out.push_back(HexDigit(c & 0x0f));
    }
    return out;
}

std::string DecodeCookieValue(const std::string& value) {
    if (value.size() % 2 != 0) {
        return "";
    }
    std::string out;
    out.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        int hi = HexValue(value[i]);
        int lo = HexValue(value[i + 1]);
        if (hi < 0 || lo < 0) {
            return "";
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

bool IsAuthorized(httpd_req_t* req) {
    if (req == nullptr) {
        return false;
    }
    char value[256] = {0};
    size_t len = sizeof(value) - 1;
    if (httpd_req_get_cookie_val(req, kCookieName, value, &len) == ESP_OK && len > 1) {
        // val_size 返回的是含结尾 null 的长度，实际值长度要减一
        return DecodeCookieValue(std::string(value, len - 1)) == GetPassword();
    }
    // 兼容 curl / 脚本：Bearer 头直接携带密码
    char header[160] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK) {
        std::string auth(header);
        const std::string prefix = "Bearer ";
        if (auth.compare(0, prefix.size(), prefix) == 0) {
            return auth.substr(prefix.size()) == GetPassword();
        }
    }
    return false;
}

void SetCookie(httpd_req_t* req, const std::string& password) {
    // httpd 只保存头部指针直到响应发出，因此头部字符串必须比函数调用活得更久。
    // 同一 httpd 任务串行处理请求，thread_local 缓冲足够安全。
    static thread_local std::string cookie;
    cookie = std::string(kCookieName) + "=" + EncodeCookieValue(password) +
             "; Path=/; HttpOnly; SameSite=Strict; Max-Age=604800";
    httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
}

void RedirectToLogin(httpd_req_t* req, const std::string& next) {
    std::string encoded;
    for (char c : next) {
        if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
            encoded.push_back(c);
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            encoded += buf;
        }
    }
    std::string location = "/login?next=" + encoded;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location.c_str());
    httpd_resp_send(req, "", 0);
}

}  // namespace web_auth
