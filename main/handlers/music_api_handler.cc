#include "music_api_handler.h"
#include "handler_registry.h"
#include "mcp_server.h"
#include "application.h"
#include "board.h"
#include "settings.h"
#include "cjson_utils.h"
#include <esp_http_server.h>
#include <cJSON.h>
#include <cstdlib>
#include <cstring>
#include <string>

constexpr const char* kWebUiToken = "xiaozhi";

namespace {

constexpr int kTracksPageSize = 20;

bool IsMusicAuthorized(httpd_req_t* req) {
    constexpr size_t kTokenSize = 128;
    char token[kTokenSize];
    if (httpd_req_get_hdr_value_str(req, "Authorization", token, sizeof(token)) != ESP_OK) {
        return false;
    }
    std::string header(token);
    if (header.size() < 8 || header.compare(0, 7, "Bearer ") != 0) {
        return false;
    }
    return header.substr(7) == kWebUiToken;
}

esp_err_t SendJson(httpd_req_t* req, cJSON* root, const char* status = "200 OK") {
    CJsonUniquePtr owned(root);
    CJsonStringUniquePtr printed(cJSON_PrintUnformatted(root));
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, HTTPD_TYPE_JSON);
    const char* body = printed ? printed.get() : "{}";
    return httpd_resp_send(req, body, strlen(body));
}

esp_err_t SendError(httpd_req_t* req, const char* status, const char* message) {
    CJsonUniquePtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "status", "error");
    cJSON_AddStringToObject(root.get(), "error", message);
    return SendJson(req, root.release(), status);
}

bool ReadBody(httpd_req_t* req, std::string& body, size_t max_len) {
    body.clear();
    if (req->content_len > max_len) {
        return false;
    }
    if (req->content_len == 0) {
        return true;
    }
    body.resize(req->content_len);
    int ret = httpd_req_recv(req, body.data(), req->content_len);
    if (ret <= 0) {
        return false;
    }
    body.resize(static_cast<size_t>(ret));
    return true;
}

// 读取 NVS 中第 index 条音源，返回其 JSON 字符串
bool LoadSourceJson(int index, std::string& out) {
    Settings settings("music", true);
    CJsonUniquePtr sources(cJSON_Parse(settings.GetString("sources", "[]").c_str()));
    if (sources == nullptr || !cJSON_IsArray(sources.get())) {
        return false;
    }
    cJSON* item = cJSON_GetArrayItem(sources.get(), index);
    if (item == nullptr) {
        return false;
    }
    CJsonStringUniquePtr printed(cJSON_PrintUnformatted(item));
    if (!printed) {
        return false;
    }
    out = printed.get();
    return true;
}

int ParseIntQuery(httpd_req_t* req, const char* key, int fallback) {
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return fallback;
    }
    char value[16];
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        return fallback;
    }
    return atoi(value);
}

const char* ParseMode(const char* value) {
    return (value != nullptr && strcmp(value, "shuffle") == 0) ? "shuffle" : "sequential";
}

// 旧数据迁移：base_url 只保留 scheme://host[:port]，目录路径归 path
bool NormalizeSources(cJSON* sources) {
    bool changed = false;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, sources) {
        auto base = cJSON_GetObjectItem(item, "base_url");
        auto path = cJSON_GetObjectItem(item, "path");
        if (!cJSON_IsString(base)) {
            continue;
        }
        std::string base_value = base->valuestring;
        std::string origin = McpServer::UrlOrigin(base_value);
        std::string base_path = base_value.substr(origin.size());
        if (origin != base_value) {
            cJSON_SetValuestring(base, origin.c_str());
            changed = true;
        }
        if (cJSON_IsString(path)) {
            std::string path_value = path->valuestring;
            std::string normalized = path_value;
            if (normalized.empty()) {
                normalized = base_path.empty() ? "/" : base_path;
            } else if (normalized.front() != '/') {
                normalized.insert(normalized.begin(), '/');
            }
            if (normalized != path_value) {
                cJSON_SetValuestring(path, normalized.c_str());
                changed = true;
            }
        }
    }
    return changed;
}

void AddStatusFields(cJSON* root, const Application::MusicStatus& status) {
    cJSON_AddBoolToObject(root, "active", status.active);
    cJSON_AddBoolToObject(root, "paused", status.paused);
    cJSON_AddBoolToObject(root, "single", status.single);
    cJSON_AddNumberToObject(root, "source", status.source_index);
    cJSON_AddNumberToObject(root, "track_index", status.track_index);
    cJSON_AddNumberToObject(root, "total", status.total);
    cJSON_AddBoolToObject(root, "truncated", status.truncated);
    cJSON_AddStringToObject(root, "mode",
                            status.mode == Application::MusicPlayMode::kShuffle ? "shuffle"
                                                                                : "sequential");
    cJSON_AddStringToObject(root, "title", status.title.c_str());
    cJSON_AddNumberToObject(root, "position_ms", status.position_ms);
    cJSON_AddNumberToObject(root, "duration_ms", status.duration_ms);
    cJSON_AddNumberToObject(root, "volume", status.volume);
}

esp_err_t MusicStatusHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    CJsonUniquePtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "status", "ok");
    AddStatusFields(root.get(), Application::GetInstance().GetMusicStatus());
    return SendJson(req, root.release());
}

esp_err_t MusicTracksHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    int source_index = ParseIntQuery(req, "source", 0);
    std::string source_json;
    if (!LoadSourceJson(source_index, source_json)) {
        return SendError(req, "404 Not Found", "source not found");
    }

    int page = ParseIntQuery(req, "page", 1);
    if (page < 1) {
        page = 1;
    }
    bool truncated = false;
    auto tracks = McpServer::GetInstance().ListMusicTracks(source_json, &truncated);
    if (!tracks) {
        return SendJson(req, [&]() {
            CJsonUniquePtr root(cJSON_CreateObject());
            cJSON_AddStringToObject(root.get(), "status", "error");
            cJSON_AddStringToObject(root.get(), "error", tracks.error().c_str());
            return root.release();
        }());
    }

    int total = static_cast<int>(tracks->size());
    int pages = (total + kTracksPageSize - 1) / kTracksPageSize;
    if (pages < 1) {
        pages = 1;
    }
    if (page > pages) {
        page = pages;
    }
    int first = (page - 1) * kTracksPageSize;
    int last = first + kTracksPageSize;
    if (last > total) {
        last = total;
    }

    CJsonUniquePtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "status", "ok");
    cJSON_AddNumberToObject(root.get(), "total", total);
    cJSON_AddNumberToObject(root.get(), "page", page);
    cJSON_AddNumberToObject(root.get(), "pages", pages);
    cJSON_AddNumberToObject(root.get(), "page_size", kTracksPageSize);
    cJSON_AddBoolToObject(root.get(), "truncated", truncated);
    cJSON* list = cJSON_AddArrayToObject(root.get(), "tracks");
    for (int i = first; i < last; ++i) {
        cJSON* track = cJSON_CreateObject();
        cJSON_AddNumberToObject(track, "index", i);
        cJSON_AddStringToObject(track, "title", (*tracks)[i].title.c_str());
        cJSON_AddItemToArray(list, track);
    }
    return SendJson(req, root.release());
}

// POST /api/music/source  {"index":0,"mode":"shuffle"}
esp_err_t MusicPlaySourceHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    std::string body;
    if (!ReadBody(req, body, 256)) {
        return SendError(req, "400 Bad Request", "body too large");
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (root == nullptr || !cJSON_IsObject(root.get())) {
        return SendError(req, "400 Bad Request", "invalid json");
    }
    auto index_item = cJSON_GetObjectItem(root.get(), "index");
    if (index_item == nullptr || !cJSON_IsNumber(index_item)) {
        return SendError(req, "400 Bad Request", "missing index");
    }
    int source_index = index_item->valueint;
    // 兼容旧请求：显式传 mode 时先写入 NVS；否则直接用 NVS 里保存的模式
    auto mode_item = cJSON_GetObjectItem(root.get(), "mode");
    if (cJSON_IsString(mode_item)) {
        Application::GetInstance().SetMusicMode(
            strcmp(mode_item->valuestring, "shuffle") == 0 ? Application::MusicPlayMode::kShuffle
                                                           : Application::MusicPlayMode::kSequential);
    }
    Application::SaveSelectedSource(source_index);
    auto start_item = cJSON_GetObjectItem(root.get(), "start");
    int start_index = cJSON_IsNumber(start_item) ? start_item->valueint : 0;

    std::string source_json;
    if (!LoadSourceJson(source_index, source_json)) {
        return SendError(req, "404 Not Found", "source not found");
    }
    bool truncated = false;
    auto tracks = McpServer::GetInstance().ListMusicTracks(source_json, &truncated);
    if (!tracks) {
        return SendError(req, "502 Bad Gateway", tracks.error().c_str());
    }
    if (tracks->empty()) {
        return SendError(req, "404 Not Found", "no audio tracks in source");
    }

    int total = static_cast<int>(tracks->size());
    Application::GetInstance().PlayMusicPlaylist(
        std::move(*tracks), source_index,
        Application::GetInstance().GetMusicStatus().mode, start_index, truncated);

    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "playing");
    cJSON_AddNumberToObject(response.get(), "source", source_index);
    cJSON_AddNumberToObject(response.get(), "total", total);
    cJSON_AddBoolToObject(response.get(), "truncated", truncated);
    return SendJson(req, response.release());
}

// POST /api/music/track  {"index":3}
esp_err_t MusicPlayTrackHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    std::string body;
    if (!ReadBody(req, body, 128)) {
        return SendError(req, "400 Bad Request", "body too large");
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    auto index_item = root ? cJSON_GetObjectItem(root.get(), "index") : nullptr;
    if (index_item == nullptr || !cJSON_IsNumber(index_item)) {
        return SendError(req, "400 Bad Request", "missing index");
    }
    Application::GetInstance().PlayMusicTrackAt(index_item->valueint);
    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "playing");
    return SendJson(req, response.release());
}

esp_err_t MusicNextHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    Application::GetInstance().PlayNextMusic();
    CJsonUniquePtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "status", "ok");
    return SendJson(req, root.release());
}

esp_err_t MusicStopHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    Application::GetInstance().StopMusicPlayback();
    CJsonUniquePtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "status", "ok");
    return SendJson(req, root.release());
}

esp_err_t MusicPrevHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    Application::GetInstance().PlayPrevMusic();
    CJsonUniquePtr root(cJSON_CreateObject());
    cJSON_AddStringToObject(root.get(), "status", "ok");
    return SendJson(req, root.release());
}

// POST /api/music/mode  {"mode":"shuffle"}
esp_err_t MusicModeHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    std::string body;
    if (!ReadBody(req, body, 128)) {
        return SendError(req, "400 Bad Request", "body too large");
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    auto mode_item = root ? cJSON_GetObjectItem(root.get(), "mode") : nullptr;
    const char* mode = ParseMode(cJSON_IsString(mode_item) ? mode_item->valuestring : nullptr);
    Application::GetInstance().SetMusicMode(
        strcmp(mode, "shuffle") == 0 ? Application::MusicPlayMode::kShuffle
                                     : Application::MusicPlayMode::kSequential);
    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "ok");
    cJSON_AddStringToObject(response.get(), "mode", mode);
    return SendJson(req, response.release());
}

// POST /api/music/volume  {"volume":40}
esp_err_t MusicVolumeHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    std::string body;
    if (!ReadBody(req, body, 128)) {
        return SendError(req, "400 Bad Request", "body too large");
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    auto volume_item = root ? cJSON_GetObjectItem(root.get(), "volume") : nullptr;
    if (volume_item == nullptr || !cJSON_IsNumber(volume_item)) {
        return SendError(req, "400 Bad Request", "missing volume");
    }
    int volume = volume_item->valueint;
    if (volume < 0 || volume > 100) {
        return SendError(req, "400 Bad Request", "invalid volume");
    }
    Board::GetInstance().GetAudioCodec()->SetOutputVolume(volume);
    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "ok");
    cJSON_AddNumberToObject(response.get(), "volume", volume);
    return SendJson(req, response.release());
}

// POST /api/music/pause
esp_err_t MusicPauseHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    Application::GetInstance().PauseMusic();
    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "ok");
    return SendJson(req, response.release());
}

// POST /api/music/resume
esp_err_t MusicResumeHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    Application::GetInstance().ResumeMusic();
    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "ok");
    return SendJson(req, response.release());
}

// POST /api/music/seek  {"time_ms":65000}
esp_err_t MusicSeekHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    std::string body;
    if (!ReadBody(req, body, 128)) {
        return SendError(req, "400 Bad Request", "body too large");
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (root == nullptr || !cJSON_IsObject(root.get())) {
        return SendError(req, "400 Bad Request", "invalid json");
    }
    auto time_item = cJSON_GetObjectItem(root.get(), "time_ms");
    if (time_item == nullptr || !cJSON_IsNumber(time_item)) {
        return SendError(req, "400 Bad Request", "missing time_ms");
    }
    int time_ms = time_item->valueint;
    if (time_ms < 0) {
        return SendError(req, "400 Bad Request", "invalid time_ms");
    }
    Application::GetInstance().SeekMusic(static_cast<uint32_t>(time_ms));
    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "ok");
    return SendJson(req, response.release());
}

// POST /api/music/play  {"source":{...}}  单曲播放，AI 与调试用
esp_err_t MusicPlayHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    std::string body;
    if (!ReadBody(req, body, 1024)) {
        return SendError(req, "400 Bad Request", "body too large");
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    auto source_item = root ? cJSON_GetObjectItem(root.get(), "source") : nullptr;
    if (source_item == nullptr) {
        return SendError(req, "400 Bad Request", "missing source");
    }
    std::string source;
    if (cJSON_IsString(source_item)) {
        source = source_item->valuestring;
    } else if (cJSON_IsObject(source_item)) {
        CJsonStringUniquePtr printed(cJSON_PrintUnformatted(source_item));
        source = printed ? printed.get() : "";
    }
    if (source.empty()) {
        return SendError(req, "400 Bad Request", "invalid source");
    }
    auto result = McpServer::GetInstance().PlayMusic(source);
    if (!result) {
        return SendError(req, "400 Bad Request", result.error().c_str());
    }
    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "playing");
    return SendJson(req, response.release());
}

// POST /api/music/list  {"source":{...}}   WebDAV 目录原始条目
esp_err_t MusicListHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    std::string body;
    if (!ReadBody(req, body, 1024)) {
        return SendError(req, "400 Bad Request", "body too large");
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    auto source_item = root ? cJSON_GetObjectItem(root.get(), "source") : nullptr;
    if (source_item == nullptr) {
        return SendError(req, "400 Bad Request", "missing source");
    }
    std::string source;
    if (cJSON_IsString(source_item)) {
        source = source_item->valuestring;
    } else if (cJSON_IsObject(source_item)) {
        CJsonStringUniquePtr printed(cJSON_PrintUnformatted(source_item));
        source = printed ? printed.get() : "";
    }
    auto result = McpServer::GetInstance().ListWebdav(source);
    if (!result) {
        return SendError(req, "502 Bad Gateway", result.error().c_str());
    }
    CJsonUniquePtr entries(cJSON_Parse(result->c_str()));
    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "ok");
    cJSON_AddItemToObject(response.get(), "entries", entries.release());
    return SendJson(req, response.release());
}

// GET/POST /api/music/sources
esp_err_t MusicSourcesHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }

    Settings settings("music", true);

    if (req->method == HTTP_GET) {
        CJsonUniquePtr sources(cJSON_Parse(settings.GetString("sources", "[]").c_str()));
        if (sources == nullptr || !cJSON_IsArray(sources.get())) {
            sources.reset(cJSON_CreateArray());
        }
        if (NormalizeSources(sources.get())) {
            CJsonStringUniquePtr printed(cJSON_PrintUnformatted(sources.get()));
            if (printed) {
                settings.SetString("sources", printed.get());
            }
        }
        CJsonUniquePtr response(cJSON_CreateObject());
        cJSON_AddStringToObject(response.get(), "status", "ok");
        cJSON_AddNumberToObject(response.get(), "selected_index",
                                Application::LoadSelectedSource());
        cJSON_AddItemToObject(response.get(), "sources", sources.release());
        return SendJson(req, response.release());
    }

    std::string body;
    if (!ReadBody(req, body, 2048)) {
        return SendError(req, "400 Bad Request", "body too large");
    }
    CJsonUniquePtr sources(cJSON_Parse(body.c_str()));
    if (sources == nullptr || !cJSON_IsArray(sources.get())) {
        return SendError(req, "400 Bad Request", "expected source array");
    }
    NormalizeSources(sources.get());
    CJsonStringUniquePtr printed(cJSON_PrintUnformatted(sources.get()));
    settings.SetString("sources", printed ? printed.get() : "[]");

    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "ok");
    return SendJson(req, response.release());
}

// POST /api/music/search  {"keyword":"xxx","source":0}
esp_err_t MusicSearchHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    std::string body;
    if (!ReadBody(req, body, 512)) {
        return SendError(req, "400 Bad Request", "body too large");
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (root == nullptr || !cJSON_IsObject(root.get())) {
        return SendError(req, "400 Bad Request", "invalid json");
    }
    auto keyword_item = cJSON_GetObjectItem(root.get(), "keyword");
    if (keyword_item == nullptr || !cJSON_IsString(keyword_item) || strlen(keyword_item->valuestring) == 0) {
        return SendError(req, "400 Bad Request", "missing keyword");
    }
    std::string keyword = keyword_item->valuestring;
    int source_index = 0;
    auto source_item = cJSON_GetObjectItem(root.get(), "source");
    if (source_item != nullptr && cJSON_IsNumber(source_item)) {
        source_index = source_item->valueint;
    }
    auto result = McpServer::GetInstance().SearchMusic(keyword, source_index);
    if (!result) {
        return SendError(req, "404 Not Found", result.error().c_str());
    }
    std::string result_str;
    if (std::holds_alternative<std::string>(*result)) {
        result_str = std::get<std::string>(*result);
    } else {
        return SendError(req, "500 Internal Error", "unexpected search result type");
    }
    CJsonUniquePtr response(cJSON_Parse(result_str.c_str()));
    if (response == nullptr || !cJSON_IsArray(response.get())) {
        return SendError(req, "500 Internal Error", "unexpected search result format");
    }
    CJsonUniquePtr root_resp(cJSON_CreateObject());
    cJSON_AddStringToObject(root_resp.get(), "status", "ok");
    cJSON_AddItemToObject(root_resp.get(), "matches", response.release());
    return SendJson(req, root_resp.release());
}

// POST /api/music/play_track  {"source":0,"index":3}
esp_err_t MusicPlayTrackAtHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    std::string body;
    if (!ReadBody(req, body, 128)) {
        return SendError(req, "400 Bad Request", "body too large");
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    if (root == nullptr || !cJSON_IsObject(root.get())) {
        return SendError(req, "400 Bad Request", "invalid json");
    }
    auto source_item = cJSON_GetObjectItem(root.get(), "source");
    auto index_item = cJSON_GetObjectItem(root.get(), "index");
    if (source_item == nullptr || !cJSON_IsNumber(source_item) ||
        index_item == nullptr || !cJSON_IsNumber(index_item)) {
        return SendError(req, "400 Bad Request", "missing source or index");
    }
    int source_index = source_item->valueint;
    int track_index = index_item->valueint;

    std::string source_json;
    if (!LoadSourceJson(source_index, source_json)) {
        return SendError(req, "404 Not Found", "source not found");
    }
    bool truncated = false;
    auto tracks = McpServer::GetInstance().ListMusicTracks(source_json, &truncated);
    if (!tracks) {
        return SendError(req, "502 Bad Gateway", tracks.error().c_str());
    }
    if (track_index < 0 || track_index >= static_cast<int>(tracks->size())) {
        return SendError(req, "400 Bad Request", "track index out of range");
    }
    auto mode = Application::GetInstance().GetMusicStatus().mode;
    Application::GetInstance().PlayMusicPlaylist(std::move(*tracks), source_index, mode, track_index, truncated);

    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "playing");
    cJSON_AddNumberToObject(response.get(), "source", source_index);
    cJSON_AddNumberToObject(response.get(), "track", track_index);
    return SendJson(req, response.release());
}

// POST /api/music/selected_source  {"index":0}
esp_err_t MusicSelectedSourceHandler(httpd_req_t* req) {
    if (!IsMusicAuthorized(req)) {
        return SendError(req, "401 Unauthorized", "unauthorized");
    }
    std::string body;
    if (!ReadBody(req, body, 128)) {
        return SendError(req, "400 Bad Request", "body too large");
    }
    CJsonUniquePtr root(cJSON_Parse(body.c_str()));
    auto index_item = root ? cJSON_GetObjectItem(root.get(), "index") : nullptr;
    if (index_item == nullptr || !cJSON_IsNumber(index_item)) {
        return SendError(req, "400 Bad Request", "missing index");
    }
    Application::SaveSelectedSource(index_item->valueint);
    CJsonUniquePtr response(cJSON_CreateObject());
    cJSON_AddStringToObject(response.get(), "status", "ok");
    return SendJson(req, response.release());
}

}  // namespace

void MusicApiHandler::RegisterHandlers(HandlerRegistry& registry) {
    const struct {
        const char* uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t*);
        const char* description;
    } routes[] = {
        {"/api/music/sources", HTTP_GET, MusicSourcesHandler, "List music sources"},
        {"/api/music/sources", HTTP_POST, MusicSourcesHandler, "Save music sources"},
        {"/api/music/tracks", HTTP_GET, MusicTracksHandler, "List tracks of a source"},
        {"/api/music/source", HTTP_POST, MusicPlaySourceHandler, "Play a whole source"},
        {"/api/music/track", HTTP_POST, MusicPlayTrackHandler, "Play one track"},
        {"/api/music/next", HTTP_POST, MusicNextHandler, "Next track"},
        {"/api/music/prev", HTTP_POST, MusicPrevHandler, "Previous track"},
        {"/api/music/stop", HTTP_POST, MusicStopHandler, "Stop playback"},
        {"/api/music/pause", HTTP_POST, MusicPauseHandler, "Pause playback"},
        {"/api/music/resume", HTTP_POST, MusicResumeHandler, "Resume playback"},
        {"/api/music/seek", HTTP_POST, MusicSeekHandler, "Seek playback"},
        {"/api/music/mode", HTTP_POST, MusicModeHandler, "Set playback mode"},
        {"/api/music/selected_source", HTTP_POST, MusicSelectedSourceHandler, "Set selected source"},
        {"/api/music/volume", HTTP_POST, MusicVolumeHandler, "Set volume"},
        {"/api/music/status", HTTP_GET, MusicStatusHandler, "Playback status"},
        {"/api/music/play", HTTP_POST, MusicPlayHandler, "Play a single track URL"},
        {"/api/music/list", HTTP_POST, MusicListHandler, "List WebDAV directory entries"},
        {"/api/music/search", HTTP_POST, MusicSearchHandler, "Search tracks by keyword"},
        {"/api/music/play_track", HTTP_POST, MusicPlayTrackAtHandler, "Play a specific track by index"},
    };
    for (const auto& route : routes) {
        registry.Register({
            .uri = route.uri,
            .method = route.method,
            .handler = route.handler,
            .user_ctx = nullptr,
            .is_websocket = false,
        }, route.description);
    }
}
