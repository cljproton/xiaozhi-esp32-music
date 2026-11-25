#include "ebook_api_handler.h"
#include "handler_registry.h"
#include "web_auth.h"
#include "application.h"
#include "ebook/ebook_player.h"
#include "tts/tts_client.h"
#include "mcp_server.h"
#include "settings.h"
#include "cjson_utils.h"

#include <esp_http_server.h>
#include <cJSON.h>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace ebook_api {
cJSON* ListSourceBooks(const std::string& dir_url, const std::string& username,
                       const std::string& password, std::string& error);
}

namespace {

esp_err_t SendJson(httpd_req_t* req, cJSON* root, const char* status = "200 OK") {
    CJsonStringUniquePtr printed(cJSON_PrintUnformatted(root));
    cJSON_Delete(root);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    const char* body = printed ? printed.get() : "{}";
    return httpd_resp_send(req, body, strlen(body));
}

esp_err_t SendError(httpd_req_t* req, const char* status, const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "error", message);
    return SendJson(req, root, status);
}

bool ReadBody(httpd_req_t* req, std::string& body, size_t max_len) {
    body.clear();
    if (req->content_len <= 0 || req->content_len > max_len) return false;
    body.resize(req->content_len);
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, &body[received], req->content_len - received);
        if (n <= 0) return false;
        received += n;
    }
    return true;
}

esp_err_t HandleStatus(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");
    auto st = EbookPlayer::GetInstance().GetStatus();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddBoolToObject(root, "active", st.active);
    cJSON_AddBoolToObject(root, "paused", st.paused);
    cJSON_AddStringToObject(root, "book", st.book_name.c_str());
    cJSON_AddNumberToObject(root, "paragraph", st.paragraph);
    cJSON_AddNumberToObject(root, "total", st.total);
    return SendJson(req, root);
}

// POST /api/ebook/list  {"url":<webdav dir>,"username","password"}
esp_err_t HandleList(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");
    std::string body;
    if (!ReadBody(req, body, 2048)) return SendError(req, "400 Bad Request", "bad body");
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendError(req, "400 Bad Request", "invalid JSON");
    auto url = cJSON_GetObjectItem(root, "url");
    auto user = cJSON_GetObjectItem(root, "username");
    auto pass = cJSON_GetObjectItem(root, "password");
    if (!cJSON_IsString(url) || !url->valuestring[0]) {
        cJSON_Delete(root);
        return SendError(req, "400 Bad Request", "missing url");
    }
    std::string u = cJSON_IsString(user) ? user->valuestring : "";
    std::string p = cJSON_IsString(pass) ? pass->valuestring : "";
    std::string error;
    cJSON* books = ebook_api::ListSourceBooks(url->valuestring, u, p, error);
    cJSON_Delete(root);
    if (books == nullptr) return SendError(req, "502 Bad Gateway", error.c_str());

    cJSON* res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", "ok");
    cJSON_AddItemToObject(res, "books", books);
    return SendJson(req, res);
}

esp_err_t HandlePlay(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");
    std::string body;
    if (!ReadBody(req, body, 2048)) return SendError(req, "400 Bad Request", "bad body");
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendError(req, "400 Bad Request", "invalid JSON");
    auto url = cJSON_GetObjectItem(root, "url");
    auto user = cJSON_GetObjectItem(root, "username");
    auto pass = cJSON_GetObjectItem(root, "password");
    bool ok = false;
    std::string detail = "missing url";
    if (cJSON_IsString(url) && url->valuestring[0]) {
        std::string u = cJSON_IsString(user) ? user->valuestring : "";
        std::string p = cJSON_IsString(pass) ? pass->valuestring : "";
        ok = Application::GetInstance().PlayEbook(url->valuestring, u, p);
        if (!ok) {
            auto st = EbookPlayer::GetInstance().GetStatus();
            detail = "failed to load book";
        }
    }
    cJSON_Delete(root);
    cJSON* res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", ok ? "ok" : "error");
    if (!ok) cJSON_AddStringToObject(res, "error", detail.c_str());
    return SendJson(req, res, ok ? "200 OK" : "400 Bad Request");
}

esp_err_t HandleControl(httpd_req_t* req, int cmd) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");
    bool ok = false;
    if (cmd == 0) { Application::GetInstance().StopEbook(); ok = true; }
    else if (cmd == 1) ok = Application::GetInstance().EbookPause();
    else if (cmd == 2) ok = Application::GetInstance().EbookResume();
    else if (cmd == 3) ok = Application::GetInstance().EbookNext();
    cJSON* res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", ok ? "ok" : "error");
    return SendJson(req, res);
}

esp_err_t HandleStop(httpd_req_t* req) { return HandleControl(req, 0); }
esp_err_t HandlePause(httpd_req_t* req) { return HandleControl(req, 1); }
esp_err_t HandleResume(httpd_req_t* req) { return HandleControl(req, 2); }
esp_err_t HandleNext(httpd_req_t* req) { return HandleControl(req, 3); }

esp_err_t HandleTtsConfigGet(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");
    size_t count = 0;
    auto voices = TtsEdgeVoices(&count);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "provider", TtsGetProvider().c_str());
    cJSON_AddStringToObject(root, "voice", TtsGetVoice().c_str());
    cJSON_AddNumberToObject(root, "speed", TtsGetSpeed());
    cJSON_AddStringToObject(root, "url", TtsGetHttpUrl().c_str());
    cJSON_AddStringToObject(root, "api_key", TtsGetApiKey().c_str());
    Settings src("ebook", false);
    cJSON_AddStringToObject(root, "source_url", src.GetString("source_url", "").c_str());
    cJSON_AddStringToObject(root, "source_user", src.GetString("source_user", "").c_str());
    cJSON_AddStringToObject(root, "source_pass", src.GetString("source_pass", "").c_str());
    cJSON* arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; ++i) cJSON_AddItemToArray(arr, cJSON_CreateString(voices[i]));
    cJSON_AddItemToObject(root, "voices", arr);
    return SendJson(req, root);
}

esp_err_t HandleTtsConfigPost(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");
    std::string body;
    if (!ReadBody(req, body, 4096)) return SendError(req, "400 Bad Request", "bad body");
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendError(req, "400 Bad Request", "invalid JSON");
    auto provider = cJSON_GetObjectItem(root, "provider");
    auto voice = cJSON_GetObjectItem(root, "voice");
    auto speed = cJSON_GetObjectItem(root, "speed");
    auto url = cJSON_GetObjectItem(root, "url");
    auto key = cJSON_GetObjectItem(root, "api_key");
    if (cJSON_IsString(provider)) TtsSetProvider(provider->valuestring);
    if (cJSON_IsString(voice)) TtsSetVoice(voice->valuestring);
    if (cJSON_IsNumber(speed)) TtsSetSpeed(speed->valueint);
    if (cJSON_IsString(url)) TtsSetHttpUrl(url->valuestring);
    if (cJSON_IsString(key)) TtsSetApiKey(key->valuestring);
    auto s_url = cJSON_GetObjectItem(root, "source_url");
    auto s_user = cJSON_GetObjectItem(root, "source_user");
    auto s_pass = cJSON_GetObjectItem(root, "source_pass");
    if (cJSON_IsString(s_url) || cJSON_IsString(s_user) || cJSON_IsString(s_pass)) {
        Settings src("ebook", true);
        if (cJSON_IsString(s_url)) src.SetString("source_url", s_url->valuestring);
        if (cJSON_IsString(s_user)) src.SetString("source_user", s_user->valuestring);
        if (cJSON_IsString(s_pass)) src.SetString("source_pass", s_pass->valuestring);
    }
    cJSON_Delete(root);
    cJSON* res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", "ok");
    return SendJson(req, res);
}

// POST /api/ebook/tts-test  {"text","voice","speed"}
esp_err_t HandleTtsTest(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");
    std::string body;
    if (!ReadBody(req, body, 2048)) return SendError(req, "400 Bad Request", "bad body");
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) return SendError(req, "400 Bad Request", "invalid JSON");
    auto text = cJSON_GetObjectItem(root, "text");
    auto voice = cJSON_GetObjectItem(root, "voice");
    auto speed = cJSON_GetObjectItem(root, "speed");
    std::string t = cJSON_IsString(text) ? text->valuestring : "你好，这是语音测试";
    std::string v = cJSON_IsString(voice) ? voice->valuestring : TtsGetVoice();
    int sp = cJSON_IsNumber(speed) ? speed->valueint : 0;
    if (sp < -50 || sp > 50) sp = 0;
    cJSON_Delete(root);
    Application::GetInstance().TestEbookTts(t, v, sp);  // async, never block HTTP handler
    cJSON* res = cJSON_CreateObject();
    cJSON_AddStringToObject(res, "status", "ok");
    return SendJson(req, res);
}

}  // namespace

namespace ebook_api {

// Percent-decode a WebDAV href filename into readable UTF-8 text.
static std::string UrlDecodeName(const std::string& in) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    // Double-encoded names (e.g. %255B -> %5B) need a second pass.
    if (out.find('%') != std::string::npos) return UrlDecodeName(out);
    return out;
}

cJSON* ListSourceBooks(const std::string& full, const std::string& u,
                       const std::string& p, std::string& error) {
    size_t scheme = full.find("://");
    size_t slash = scheme == std::string::npos ? std::string::npos : full.find('/', scheme + 3);
    std::string origin = slash == std::string::npos ? full : full.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : full.substr(slash);
    if (!path.empty() && path.back() != '/') path += '/';

    cJSON* src = cJSON_CreateObject();
    cJSON_AddStringToObject(src, "type", "webdav");
    cJSON_AddStringToObject(src, "base_url", origin.c_str());
    cJSON_AddStringToObject(src, "username", u.c_str());
    cJSON_AddStringToObject(src, "password", p.c_str());
    cJSON_AddStringToObject(src, "path", path.c_str());
    char* src_str = cJSON_PrintUnformatted(src);
    cJSON_Delete(src);
    auto listing = McpServer::GetInstance().ListWebdav(src_str ? src_str : "{}");
    free(src_str);
    if (!listing) {
        error = listing.error();
        return nullptr;
    }
    cJSON* entries = cJSON_Parse(listing->c_str());
    cJSON* books = cJSON_CreateArray();
    if (entries && cJSON_IsArray(entries)) {
        for (int i = 0; i < cJSON_GetArraySize(entries); ++i) {
            cJSON* e = cJSON_GetArrayItem(entries, i);
            if (!cJSON_IsString(e) || !e->valuestring) continue;
            std::string href = e->valuestring;
            size_t dot = href.find_last_of('.');
            if (dot == std::string::npos) continue;
            std::string ext = href.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".txt" && ext != ".epub") continue;
            cJSON* item = cJSON_CreateObject();
            std::string file_url = origin + href;
            cJSON_AddStringToObject(item, "url", file_url.c_str());
            cJSON_AddStringToObject(item, "name", UrlDecodeName(href.substr(href.find_last_of('/') + 1)).c_str());
            // Reading progress (paragraph index) stored by EbookPlayer.
            uint32_t h = 2166136261u;
            for (unsigned char c : file_url) { h ^= c; h *= 16777619u; }
            char key[16];
            snprintf(key, sizeof(key), "%08lx", (unsigned long)h);
            Settings prog("ebook", false);
            cJSON_AddNumberToObject(item, "progress", prog.GetInt(key, 0));
            cJSON_AddItemToArray(books, item);
        }
    }
    if (entries) cJSON_Delete(entries);
    return books;
}

void RegisterHandlers(HandlerRegistry& registry) {
    registry.Register({"/api/ebook/list", HTTP_POST, HandleList, nullptr}, "Ebook list");
    registry.Register({"/api/ebook/status", HTTP_GET, HandleStatus, nullptr}, "Ebook status");
    registry.Register({"/api/ebook/play", HTTP_POST, HandlePlay, nullptr}, "Ebook play");
    registry.Register({"/api/ebook/stop", HTTP_POST, HandleStop, nullptr}, "Ebook stop");
    registry.Register({"/api/ebook/pause", HTTP_POST, HandlePause, nullptr}, "Ebook pause");
    registry.Register({"/api/ebook/resume", HTTP_POST, HandleResume, nullptr}, "Ebook resume");
    registry.Register({"/api/ebook/next", HTTP_POST, HandleNext, nullptr}, "Ebook next");
    registry.Register({"/api/ebook/tts-config", HTTP_GET, HandleTtsConfigGet, nullptr}, "TTS config get");
    registry.Register({"/api/ebook/tts-config", HTTP_POST, HandleTtsConfigPost, nullptr}, "TTS config set");
    registry.Register({"/api/ebook/tts-test", HTTP_POST, HandleTtsTest, nullptr}, "TTS test");
}

}  // namespace ebook_api
