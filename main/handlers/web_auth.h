#pragma once

#include <esp_http_server.h>

#include <string>

// 网页端统一鉴权：Cookie 为主，Bearer 头兼容脚本调用。
namespace web_auth {

constexpr const char* kCookieName = "xiaozhi_session";
constexpr const char* kDefaultPassword = "xiaozhi";

std::string GetPassword();
bool IsDefaultPassword();
bool CheckPassword(const std::string& password);
bool ChangePassword(const std::string& old_password, const std::string& new_password);
bool IsAuthorized(httpd_req_t* req);
std::string EncodeCookieValue(const std::string& password);
std::string DecodeCookieValue(const std::string& value);
void SetCookie(httpd_req_t* req, const std::string& password);
void RedirectToLogin(httpd_req_t* req, const std::string& next);

}  // namespace web_auth
