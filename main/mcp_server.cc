/*
 * MCP Server Implementation
 * Revisions: 2024-11-05 (legacy) and 2026-07-28 (per-request metadata)
 */

#include "mcp_server.h"
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <optional>
#include <random>
#include <string_view>
#include <future>

#include "application.h"
#include "playback_scheduler.h"
#include "board.h"
#include "display.h"
#include "lvgl_image.h"
#include "lvgl_theme.h"
#include "settings.h"
#if CONFIG_USE_RADIO
#include "radio/radio_factory.h"
#include "radio/radio_api_handler.h"
#include "radio/radio_source.h"
#endif
#include "weather/weather_service.h"
#include "ebook/ebook_player.h"
#include "handlers/ebook_api_handler.h"
#include "settings.h"

#define TAG "MCP"

// 将 PlaybackResult 转换为 MCP ToolResult
static ToolResult ToToolResult(const PlaybackResult& r) {
    if (r.success) return true;
    return std::unexpected(r.error.empty() ? "Playback failed" : r.error);
}

McpServer::McpServer() {}

McpServer::~McpServer() = default;

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
            "Provides the real-time information of the device, including the current status of the "
            "audio speaker, screen, battery, network, and why the device last restarted.\n"
            "Use this tool for: \n"
            "1. Answering questions about current condition (e.g. what is the current volume of "
            "the audio speaker?)\n"
            "2. Answering why the device rebooted or restarted - whether it was powered on, "
            "reset by its button, or crashed. This is about the device, not about the "
            "conversation.\n"
            "3. As the first step to control the device (e.g. turn up / down the volume of the "
            "audio speaker, etc.)",
            PropertyList(), [&board](const PropertyList& properties) -> ReturnValue {
                return board.GetDeviceStatusJson();
            });

    AddTool("self.audio_speaker.set_volume",
            "Set the volume of the audio speaker. If the current volume is unknown, you must call "
            "`self.get_device_status` tool first and then call this tool.",
            PropertyList({Property("volume", kPropertyTypeInteger, 0, 100)}),
            [&board](const PropertyList& properties) -> ReturnValue {
                auto codec = board.GetAudioCodec();
                codec->SetOutputVolume(properties["volume"].value<int>());
                return true;
            });

    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness", "Set the brightness of the screen.",
                PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100)}),
                [backlight](const PropertyList& properties) -> ReturnValue {
                    uint8_t brightness =
                        static_cast<uint8_t>(properties["brightness"].value<int>());
                    backlight->SetBrightness(brightness, true);
                    return true;
                });
    }

    // Sleep mode tools
    AddTool("self.sleep.get_status",
            "Get the current sleep mode configuration including enabled state, daily shutdown time, and idle shutdown minutes.",
            PropertyList(),
            [](const PropertyList& properties) -> ReturnValue {
                Settings settings("wifi", false);
                bool sleep_mode = settings.GetBool("sleep_mode", true);
                int sleep_off_min = settings.GetInt("sleep_off_min", 0);  // 0 = disabled (default)
                int sleep_off_at = settings.GetInt("sleep_off_at", -1);  // minutes from midnight
                Settings wns("weather", false);
                std::string city = wns.GetString("city", "");
                cJSON* json = cJSON_CreateObject();
                cJSON_AddBoolToObject(json, "sleep_mode", sleep_mode);
                cJSON_AddNumberToObject(json, "sleep_off_min", sleep_off_min);
                cJSON_AddNumberToObject(json, "sleep_off_at", sleep_off_at);
                cJSON_AddStringToObject(json, "weather_city", city.c_str());
                return json;
            });

    AddTool("self.sleep.configure",
            "Configure sleep mode with all three settings at once. "
            "Call self.sleep.get_status first to see current values, then call this with the complete desired configuration. "
            "Parameters: enabled (bool), daily_shutdown (string HH:MM or empty to disable), idle_minutes (int 0-1440, 0=disabled).",
            PropertyList({
                Property("enabled", kPropertyTypeBoolean),
                Property("daily_shutdown", kPropertyTypeString, ""),
                Property("idle_minutes", kPropertyTypeInteger, 0, 0, 1440)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                bool enabled = properties["enabled"].value<bool>();
                std::string daily = properties["daily_shutdown"].value<std::string>();
                int idle = properties["idle_minutes"].value<int>();
                
                Settings settings("wifi", true);
                settings.SetBool("sleep_mode", enabled);
                
                if (!daily.empty()) {
                    int h = 0, m = 0;
                    if (sscanf(daily.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
                        return std::string("daily_shutdown invalid HH:MM");
                    }
                    settings.SetInt("sleep_off_at", h * 60 + m);
                } else {
                    settings.SetInt("sleep_off_at", -1);
                }
                settings.SetInt("sleep_off_min", idle);
                return true;
            });

    // Weather city tool
    AddTool("self.standby.set_weather_city",
            "Set city name for standby weather display. Empty string = auto by IP.",
            PropertyList({Property("city", kPropertyTypeString, "")}),
            [](const PropertyList& properties) -> ReturnValue {
                std::string city = properties["city"].value<std::string>();
                weather::SetCity(city);
                return true;
            });

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
                "Set the theme of the screen. The theme can be `light` or `dark`.",
                PropertyList({Property("theme", kPropertyTypeString)}),
                [display](const PropertyList& properties) -> ReturnValue {
                    auto theme_name = properties["theme"].value<std::string>();
                    auto& theme_manager = LvglThemeManager::GetInstance();
                    auto theme = theme_manager.GetTheme(theme_name);
                    if (theme != nullptr) {
                        display->SetTheme(theme);
                        return true;
                    }
                    return false;
                });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
                "Always remember you have a camera. If the user asks you to see something, use "
                "this tool to take a photo and then explain it.\n"
                "Args:\n"
                "  `question`: The question that you want to ask about the photo.\n"
                "Return:\n"
                "  A JSON object that provides the photo information.",
                PropertyList({Property("question", kPropertyTypeString)}),
                [](const PropertyList& properties, const McpRequestContext& context) -> ToolResult {
                    auto camera = Board::GetInstance().GetCamera();
                    if (camera == nullptr) {
                        return std::unexpected("Camera unavailable");
                    }
                    // Lower the priority to do the camera capture
                    TaskPriorityReset priority_reset(1);

                    if (!camera->Capture()) {
                        return std::unexpected("Failed to capture photo");
                    }
                    auto question = properties["question"].value<std::string>();
                    auto result = context.vision ? camera->Explain(question, context.vision->url,
                                                                   context.vision->token)
                                                 : camera->Explain(question);
                    if (!result) {
                        return std::unexpected(std::move(result.error()));
                    }
                    return std::move(*result);
                });
    }
#endif

    AddTool("self.music.sources",
            "List the WebDAV music sources currently configured on this device. "
            "Read-only: add, edit or delete sources on the device web page, not through this tool. "
            "Call this before playing music so you know which sources are available.",
            PropertyList(),
            [this](const PropertyList& properties) -> ToolResult {
                Settings settings("music", true);
                auto sources_json = settings.GetString("sources", "[]");
                CJsonUniquePtr sources(cJSON_Parse(sources_json.c_str()));
                if (sources == nullptr || !cJSON_IsArray(sources.get())) {
                    sources.reset(cJSON_CreateArray());
                }
                CJsonStringUniquePtr printed(cJSON_PrintUnformatted(sources.get()));
                std::string json = printed ? std::string(printed.get()) : std::string("[]");
                return json;
            });

    AddTool("self.music.play",
            "Play a single music track from a remote WebDAV source (远程播放). `source` is a JSON object: "
            "{\"type\":\"webdav\",\"base_url\":\"...\",\"username\":\"...\",\"password\":\"...\",\"path\":\"...\"} "
            "for a WebDAV source. "
            "Use `self.music.play_all` to play every track of a source.",
            PropertyList({Property("source", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ToolResult {
                return PlayMusic(properties["source"].value<std::string>());
            });

    AddTool("self.music.play_all",
            "Play all music from a remote WebDAV source (远程播放/网盘音乐). "
            "If `source` is omitted, the source selected on the device web page (saved on the device) is used. "
            "If `index` >= 0, plays that saved source by index (use self.music.sources to see indices). "
            "By default the saved playback mode (sequential/shuffle) is used; set `random` to true to switch to shuffle. "
            "`source` is a JSON object: "
            "{\"type\":\"webdav\",\"base_url\":\"...\",\"username\":\"...\",\"password\":\"...\",\"path\":\"...\"} "
            "for a WebDAV source.",
            PropertyList({
                Property("source", kPropertyTypeString, ""),
                Property("random", kPropertyTypeBoolean, false),
                Property("index", kPropertyTypeInteger, -1)
            }),
            [this](const PropertyList& properties) -> ToolResult {
                int index = properties["index"].value<int>();
                if (index >= 0) {
                    return PlayMusicSourceAt(index, properties["random"].value<bool>());
                }
                return PlayMusicAll(properties["source"].value<std::string>(),
                                    properties["random"].value<bool>());
            });

    AddTool("self.music.next", "Skip to the next track in the current playlist.", PropertyList(),
            [this](const PropertyList&) -> ToolResult {
                auto& scheduler = Application::GetInstance().GetPlaybackScheduler();
                auto cmd = std::make_unique<PlaybackCommand>();
                cmd->type = PlaybackCommandType::NEXT;
                cmd->priority = PlaybackPriority::MUSIC;
                return ToToolResult(scheduler.SubmitAndWait(std::move(cmd), 5000));
            });

    AddTool("self.music.prev", "Go back to the previous track.", PropertyList(),
            [this](const PropertyList&) -> ToolResult {
                auto& scheduler = Application::GetInstance().GetPlaybackScheduler();
                auto cmd = std::make_unique<PlaybackCommand>();
                cmd->type = PlaybackCommandType::PREV;
                cmd->priority = PlaybackPriority::MUSIC;
                return ToToolResult(scheduler.SubmitAndWait(std::move(cmd), 5000));
            });

    AddTool("self.music.status", "Report what music is playing right now.", PropertyList(),
            [this](const PropertyList&) -> ToolResult {
                auto status = Application::GetInstance().GetMusicStatus();
                if (!status.active) {
                    return std::string("No music is playing");
                }
                char buf[256];
                snprintf(buf, sizeof(buf), "%s, track %d of %d, %s mode",
                         status.title.c_str(), status.track_index + 1, status.total,
                         status.mode == Application::MusicPlayMode::kShuffle ? "shuffle" : "sequential");
                return std::string(buf);
            });

    AddTool("self.music.mode",
            "设置远程播放的播放顺序：shuffle 为随机播放，sequential 为顺序播放。",
            PropertyList({Property("mode", kPropertyTypeString)}),
            [](const PropertyList& properties) -> ToolResult {
                auto mode = properties["mode"].value<std::string>();
                if (mode != "shuffle" && mode != "sequential") {
                    return std::unexpected("mode 只能是 shuffle 或 sequential");
                }
                auto& scheduler = Application::GetInstance().GetPlaybackScheduler();
                auto cmd = std::make_unique<PlaybackCommand>();
                cmd->type = PlaybackCommandType::MODE;
                cmd->priority = PlaybackPriority::MUSIC;
                cmd->mode = mode == "shuffle" ? Application::MusicPlayMode::kShuffle : Application::MusicPlayMode::kSequential;
                return ToToolResult(scheduler.SubmitAndWait(std::move(cmd), 5000));
            });

    AddTool("self.music.pause", "暂停当前正在播放的远程音乐。", PropertyList(),
            [](const PropertyList&) -> ToolResult {
                auto& scheduler = Application::GetInstance().GetPlaybackScheduler();
                auto cmd = std::make_unique<PlaybackCommand>();
                cmd->type = PlaybackCommandType::PAUSE;
                cmd->priority = PlaybackPriority::MUSIC;
                return ToToolResult(scheduler.SubmitAndWait(std::move(cmd), 5000));
            });

    AddTool("self.music.resume", "恢复播放已暂停的远程音乐。", PropertyList(),
            [](const PropertyList&) -> ToolResult {
                auto& scheduler = Application::GetInstance().GetPlaybackScheduler();
                auto cmd = std::make_unique<PlaybackCommand>();
                cmd->type = PlaybackCommandType::RESUME;
                cmd->priority = PlaybackPriority::MUSIC;
                return ToToolResult(scheduler.SubmitAndWait(std::move(cmd), 5000));
            });

    AddTool("self.music.stop", "停止播放远程音乐。", PropertyList(),
            [](const PropertyList&) -> ToolResult {
                auto& scheduler = Application::GetInstance().GetPlaybackScheduler();
                auto cmd = std::make_unique<PlaybackCommand>();
                cmd->type = PlaybackCommandType::STOP;
                cmd->priority = PlaybackPriority::SYSTEM;
                return ToToolResult(scheduler.SubmitAndWait(std::move(cmd), 5000));
            });

    AddTool("self.music.seek", "跳转到指定播放位置（毫秒）。", PropertyList(
        {Property("time_ms", kPropertyTypeInteger, 0, 0, INT_MAX)}),
            [](const PropertyList& properties) -> ToolResult {
                auto& scheduler = Application::GetInstance().GetPlaybackScheduler();
                auto cmd = std::make_unique<PlaybackCommand>();
                cmd->type = PlaybackCommandType::SEEK;
                cmd->priority = PlaybackPriority::MUSIC;
                cmd->seek_ms = properties["time_ms"].value<int>();
                return ToToolResult(scheduler.SubmitAndWait(std::move(cmd), 5000));
            });

    AddTool("self.music.search",
            "在远程音源中按歌名或歌手名模糊查找音乐（对曲目标题做包含匹配）。"
            "返回匹配曲目列表（含曲目序号 index 和标题 title）。"
            "若匹配到多首，把列表读给用户并追问要播放哪一首；用户选定后用 self.music.play_track 播放。"
            "`source` 为音源序号，默认 0。",
            PropertyList({Property("keyword", kPropertyTypeString),
                          Property("source", kPropertyTypeInteger, 0)}),
            [this](const PropertyList& properties) -> ToolResult {
                return SearchMusic(properties["keyword"].value<std::string>(),
                                   properties["source"].value<int>());
            });

    AddTool("self.music.play_track",
            "播放远程音源中的某一首曲目。`source` 为音源序号，`index` 为曲目序号"
            "（self.music.search 返回的 index）。",
            PropertyList({Property("source", kPropertyTypeInteger, 0),
                          Property("index", kPropertyTypeInteger, 0, 0, 499)}),
            [this](const PropertyList& properties) -> ToolResult {
                return PlayMusicTrackAt(properties["source"].value<int>(),
                                        properties["index"].value<int>());
            });

#if CONFIG_USE_RADIO
    // 网络电台：与 self.music.* 共存，由 Application 统一做播放源互斥
    {
        auto radio_source = Application::GetInstance().GetRadioSource();
        if (!radio_source) {
            radio_source = radio::CreateRadioSource();
            Application::GetInstance().SetRadioSource(radio_source);
        }
        if (radio_source) {
            radio_source->SetPcmCallback([](std::vector<int16_t>&& pcm, int sample_rate,
                                            int channels) {
                static uint32_t pcm_id = 0;
                Application::GetInstance().GetAudioService().PushPcmPlaybackTask(
                    std::move(pcm), sample_rate, static_cast<uint32_t>(channels), ++pcm_id, 0);
            });
            Application::GetInstance().SetRadioStopHandler([radio_source]() {
                radio_source->Stop();
            });
            Application::GetInstance().SetRadioStartHandler([radio_source](const std::string& url) {
                radio_source->SetVolume(100);
                if (!radio_source->Start(url)) {
                    ESP_LOGE(TAG, "Failed to start radio stream: %s", url.c_str());
                    return;
                }
                Application::GetInstance().SetRadioPlaying(true);
                Application::GetInstance().ArmRadioWatchdog(true);
                ESP_LOGI(TAG, "Radio started: %s", url.c_str());
            });
            radio_source->SetStateCallback([radio_source](radio::RadioState state,
                                                          const std::string&) {
                auto& app = Application::GetInstance();
                if (state == radio::RadioState::kPlaying) {
                    app.ArmRadioWatchdog(false);
                } else if (state == radio::RadioState::kBuffering ||
                           state == radio::RadioState::kConnecting) {
                    app.ArmRadioWatchdog(true);
                } else if (state == radio::RadioState::kStopped ||
                           state == radio::RadioState::kError) {
                    // 可能是断流后的瞬态，自动重连由 radio source 处理；
                    // 只有显式 StopRadio() 才结束电台会话并恢复麦克风。
                    app.ArmRadioWatchdog(false);
                }
            });

            AddTool("radio.play",
                    "按名称播放已保存的网络电台（模糊匹配）。`name` 可选：为空或省略时播放第一个已保存电台。电台在设备网页端管理。",
                    PropertyList({Property("name", kPropertyTypeString, "")}),
                    [radio_source](const PropertyList& properties) -> ToolResult {
                        std::string name = properties["name"].value<std::string>();
                        auto stations = radio_source->GetStationManager().GetAllStations();
                        if (stations.empty()) {
                            std::string page = radio::RadioApiHandler::GetWebPageUrl();
                            std::string hint = page.empty()
                                ? "请先让设备联网，再用 radio.web_guide 获取网页地址。"
                                : "请在浏览器打开 " + page + " 添加电台。";
                            return std::unexpected("设备上还没有保存电台。" + hint);
                        }
                        // 用值拷贝保存选中电台：matches/stations 是局部 vector，
                        // 若存元素裸指针，作用域结束后 target 悬空，url 会被栈复用覆盖
                        // （实测语音 radio.play 拿到 "p1" 而非真实 URL）。
                        std::optional<radio::StationManager::Station> target;
                        if (!name.empty()) {
                            auto matches = radio_source->GetStationManager().FindByName(name);
                            if (matches.empty()) {
                                return std::unexpected("没有匹配的电台：" + name);
                            }
                            target = matches.front();
                        } else {
                            target = stations.front();
                        }
                        radio_source->SetVolume(100);
                        Application::GetInstance().StartRadio(target->url, true);
                        return "正在播放：" + target->name;
                    });

            AddTool("radio.stop", "停止当前电台。", PropertyList(),
                    [](const PropertyList&) -> ToolResult {
                        Application::GetInstance().StopRadio();
                        return std::string("电台已停止");
                    });

            AddTool("radio.next", "切到下一个已保存电台。", PropertyList(),
                    [radio_source](const PropertyList&) -> ToolResult {
                        auto station = radio_source->GetStationManager().GetNextStation();
                        if (!station) return std::string("没有下一个电台");
                        Application::GetInstance().StartRadio(station->url, true);
                        return "正在播放：" + station->name;
                    });

            AddTool("radio.prev", "切到上一个已保存电台。", PropertyList(),
                    [radio_source](const PropertyList&) -> ToolResult {
                        auto station = radio_source->GetStationManager().GetPrevStation();
                        if (!station) return std::string("没有上一个电台");
                        Application::GetInstance().StartRadio(station->url, true);
                        return "正在播放：" + station->name;
                    });

            AddTool("radio.web_guide",
                    "获取电台配置网页地址，供用户搜索、添加、删除电台和调节增益。",
                    PropertyList(),
                    [radio_source](const PropertyList&) -> ToolResult {
                        std::string page = radio::RadioApiHandler::GetWebPageUrl();
                        if (page.empty()) {
                            return std::unexpected("设备尚未联网，网页地址不可用");
                        }
                        int count = static_cast<int>(
                            radio_source->GetStationManager().GetAllStations().size());
                        return "电台配置页：" + page + "。当前已保存 " + std::to_string(count) +
                               "/" + std::to_string(radio::StationManager::kMaxStations) + " 台。";
                    });

            AddTool("radio.stations.list",
                    "列出设备已保存的电台（序号、名称、URL、分类、国家、增益）。",
                    PropertyList(),
                    [radio_source](const PropertyList&) -> ToolResult {
                        auto stations = radio_source->GetStationManager().GetAllStations();
                        if (stations.empty()) return std::string("设备上还没有保存电台");
                        std::string out = "已保存电台：";
                        int idx = 1;
                        for (const auto& s : stations) {
                            out += " " + std::to_string(idx++) + ". " + s.name + " - " + s.url +
                                   " [" + s.genre + "/" + s.country + "] (增益 " + std::to_string(s.gain) + "%)";
                        }
                        return out;
                    });

            AddTool("radio.stations.delete",
                    "按序号删除已保存电台，需要 confirm=true；先列列表并向用户确认。",
                    PropertyList({
                        Property("index", kPropertyTypeInteger, 1, 1, radio::StationManager::kMaxStations),
                        Property("confirm", kPropertyTypeBoolean, false),
                    }),
                    [radio_source](const PropertyList& properties) -> ToolResult {
                        int index = properties["index"].value<int>();
                        if (!properties["confirm"].value<bool>()) {
                            return std::string("删除电台需要 confirm=true，请先调用 radio.stations.list 并向用户确认。");
                        }
                        auto& mgr = radio_source->GetStationManager();
                        auto stations = mgr.GetAllStations();
                        if (index < 1 || index > static_cast<int>(stations.size())) {
                            return std::string("电台序号超出范围");
                        }
                        const auto& target = stations[static_cast<size_t>(index) - 1];
                        if (!mgr.RemoveStation(target.id)) {
                            return std::unexpected("删除失败：" + target.name);
                        }
                        return "已删除：" + target.name;
                    });
        }
    }
#endif  // CONFIG_USE_RADIO

    // E-book (TTS) playback tools
    AddTool("ebook.list", "分页列出 WebDAV 书源中的电子书（TXT），只返回书名。默认第 1 页、每页 20 本；用 page 翻页。播放时用 ebook.play 的 name 参数。",
            PropertyList({Property("url", kPropertyTypeString, ""),
                          Property("username", kPropertyTypeString, ""),
                          Property("password", kPropertyTypeString, ""),
                          Property("page", kPropertyTypeInteger, 1, 1, 1000),
                          Property("page_size", kPropertyTypeInteger, 20, 1, 50)}),
            [](const PropertyList& properties) -> ReturnValue {
                Settings src("ebook", false);
                std::string url = properties["url"].value<std::string>();
                std::string user = properties["username"].value<std::string>();
                std::string pass = properties["password"].value<std::string>();
                if (url.empty()) url = src.GetString("source_url", "");
                if (user.empty()) user = src.GetString("source_user", "");
                if (pass.empty()) pass = src.GetString("source_pass", "");
                if (url.empty()) return std::string("未配置书源，请先在网页端 /book 保存或在参数中提供 url");
                int page = properties["page"].value<int>();
                int page_size = properties["page_size"].value<int>();
                std::string error;
                int total = 0;
                // 全量列表（276 本）的 JSON 约 100KB，超过 MQTT 单次发布上限，
                // 会导致工具结果发送失败、连接断开。只取一页且只回书名。
                cJSON* books = ebook_api::ListSourceBooks(url, user, pass,
                                                          (page - 1) * page_size, page_size,
                                                          total, error);
                if (books == nullptr) return std::string("列出书目失败: ") + error;
                cJSON* slim = cJSON_CreateArray();
                for (int i = 0; i < cJSON_GetArraySize(books); ++i) {
                    cJSON* b = cJSON_GetArrayItem(books, i);
                    auto* n = cJSON_GetObjectItem(b, "name");
                    if (!cJSON_IsString(n)) continue;
                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "name", n->valuestring);
                    auto* progress = cJSON_GetObjectItem(b, "progress");
                    if (cJSON_IsNumber(progress)) {
                        cJSON_AddNumberToObject(item, "progress", progress->valueint);
                    }
                    cJSON_AddItemToArray(slim, item);
                }
                cJSON_Delete(books);
                cJSON* res = cJSON_CreateObject();
                cJSON_AddStringToObject(res, "status", "ok");
                cJSON_AddNumberToObject(res, "total", total);
                cJSON_AddNumberToObject(res, "page", page);
                cJSON_AddNumberToObject(res, "pages", (total + page_size - 1) / page_size);
                cJSON_AddItemToObject(res, "books", slim);
                return res;
            });
    AddTool("ebook.play", "朗读 TXT 电子书。可用 name 模糊匹配书名（不传 url 时使用网页端保存的书源），或直接用 url 指定完整文件地址。",
            PropertyList({Property("name", kPropertyTypeString, ""),
                          Property("url", kPropertyTypeString, ""),
                          Property("username", kPropertyTypeString, ""),
                          Property("password", kPropertyTypeString, "")}),
            [](const PropertyList& properties) -> ReturnValue {
                Settings src("ebook", false);
                std::string name = properties["name"].value<std::string>();
                std::string url = properties["url"].value<std::string>();
                std::string user = properties["username"].value<std::string>();
                std::string pass = properties["password"].value<std::string>();
                if (user.empty()) user = src.GetString("source_user", "");
                if (pass.empty()) pass = src.GetString("source_pass", "");

                // Fuzzy match by book name: resolve to a full URL from the source.
                if (url.empty() && !name.empty()) {
                    std::string source_url = src.GetString("source_url", "");
                    if (source_url.empty()) return std::string("未配置书源，请先在网页端 /book 保存");
                    std::string error;
                    int total = 0;
                    cJSON* books = ebook_api::ListSourceBooks(source_url, user, pass, 0, 0,
                                                              total, error);
                    if (books == nullptr) return std::string("列出书目失败: ") + error;
                    std::string lower_name = name;
                    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
                    std::string matched_url;
                    int matches = 0;
                    for (int i = 0; i < cJSON_GetArraySize(books); ++i) {
                        cJSON* b = cJSON_GetArrayItem(books, i);
                        auto* u = cJSON_GetObjectItem(b, "url");
                        auto* n = cJSON_GetObjectItem(b, "name");
                        if (!cJSON_IsString(u) || !cJSON_IsString(n)) continue;
                        std::string bn = n->valuestring;
                        std::transform(bn.begin(), bn.end(), bn.begin(), ::tolower);
                        if (bn.find(lower_name) != std::string::npos) {
                            matched_url = u->valuestring;
                            ++matches;
                        }
                    }
                    cJSON_Delete(books);
                    if (matches == 0) return std::string("未找到匹配书名: ") + name;
                    if (matches > 1) return std::string("书名匹配到多本书，请更精确或直接用 url");
                    url = matched_url;
                }
                if (url.empty()) return std::string("请提供 name 或 url");
                if (!Application::GetInstance().PlayEbook(url, user, pass)) {
                    return std::string("无法加载或播放电子书");
                }
                return "开始朗读电子书";
            });
    AddTool("ebook.stop", "停止电子书朗读。", PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                Application::GetInstance().StopEbook();
                return true;
            });
    AddTool("ebook.status", "查询电子书朗读状态。", PropertyList(),
            [](const PropertyList&) -> ReturnValue {
                auto st = EbookPlayer::GetInstance().GetStatus();
                cJSON* json = cJSON_CreateObject();
                cJSON_AddBoolToObject(json, "active", st.active);
                cJSON_AddBoolToObject(json, "paused", st.paused);
                cJSON_AddStringToObject(json, "book", st.book_name.c_str());
                cJSON_AddNumberToObject(json, "paragraph", st.paragraph);
                cJSON_AddNumberToObject(json, "total", st.total);
                return json;
            });

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), std::make_move_iterator(original_tools.begin()),
                  std::make_move_iterator(original_tools.end()));
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info", "Get the system information", PropertyList(),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto& board = Board::GetInstance();
                        return board.GetSystemInfoJson();
                    });

    AddUserOnlyTool("self.reboot", "Reboot the system", PropertyList(),
                    [this](const PropertyList& properties) -> ReturnValue {
                        auto& app = Application::GetInstance();
                        app.Schedule([&app]() {
                            ESP_LOGW(TAG, "User requested reboot");
                            vTaskDelay(pdMS_TO_TICKS(1000));

                            app.Reboot();
                        });
                        return true;
                    });

    // Firmware upgrade
    AddUserOnlyTool(
        "self.upgrade_firmware",
        "Upgrade firmware from a specific URL. This will download and install the firmware, then "
        "reboot the device.",
        PropertyList({Property("url", kPropertyTypeString,
                               "The URL of the firmware binary file to download and install")}),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());

            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });

            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr && display->SupportsGuiOperations()) {
        AddUserOnlyTool("self.screen.get_info",
                        "Information about the screen, including width, height, etc.",
                        PropertyList(), [display](const PropertyList& properties) -> ReturnValue {
                            cJSON* json = cJSON_CreateObject();
                            cJSON_AddNumberToObject(json, "width", display->width());
                            cJSON_AddNumberToObject(json, "height", display->height());
                            cJSON_AddBoolToObject(json, "monochrome", display->IsMonochrome());
                            return json;
                        });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool(
            "self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({Property("url", kPropertyTypeString),
                          Property("quality", kPropertyTypeInteger, 80, 1, 100)}),
            [display](const PropertyList& properties) -> ToolResult {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    return std::unexpected("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());

                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";

                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (auto opened = http->Open("POST", url); !opened) {
                    return std::unexpected("Failed to open URL: " + url + " (" +
                                           opened.error().ToString() + ")");
                }
                auto write_or_fail = [&http](const char* data,
                                             size_t size) -> std::optional<std::string> {
                    if (auto written = http->Write(data, size); !written) {
                        return written.error().ToString();
                    }
                    return std::nullopt;
                };
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header +=
                        "Content-Disposition: form-data; name=\"file\"; "
                        "filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    if (auto error = write_or_fail(file_header.c_str(), file_header.size());
                        error) {
                        http->Close();
                        return std::unexpected("Failed to upload screenshot: " + *error);
                    }
                }

                // JPEG数据
                if (auto error = write_or_fail((const char*)jpeg_data.data(), jpeg_data.size());
                    error) {
                    http->Close();
                    return std::unexpected("Failed to upload screenshot: " + *error);
                }

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    if (auto error =
                            write_or_fail(multipart_footer.c_str(), multipart_footer.size());
                        error) {
                        http->Close();
                        return std::unexpected("Failed to upload screenshot: " + *error);
                    }
                }
                if (auto error = write_or_fail("", 0); error) {
                    http->Close();
                    return std::unexpected("Failed to upload screenshot: " + *error);
                }

                auto upload_status = http->GetStatusCode();
                if (!upload_status) {
                    return std::unexpected(upload_status.error().ToString());
                }
                if (*upload_status != 200) {
                    return std::unexpected("Unexpected status code: " +
                                           std::to_string(*upload_status));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });

        AddUserOnlyTool(
            "self.screen.preview_image", "Preview an image on the screen",
            PropertyList({Property("url", kPropertyTypeString)}),
            [display](const PropertyList& properties) -> ToolResult {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (auto opened = http->Open("GET", url); !opened) {
                    return std::unexpected("Failed to open URL: " + url + " (" +
                                           opened.error().ToString() + ")");
                }
                auto status_code = http->GetStatusCode();
                if (!status_code) {
                    return std::unexpected(status_code.error().ToString());
                }
                if (*status_code != 200) {
                    return std::unexpected("Unexpected status code: " +
                                           std::to_string(*status_code));
                }

                size_t content_length = http->GetBodyLength();
                using BufferPtr = std::unique_ptr<char, decltype(&heap_caps_free)>;
                BufferPtr data(
                    static_cast<char*>(heap_caps_malloc(content_length, MALLOC_CAP_8BIT)),
                    heap_caps_free);
                if (data == nullptr) {
                    return std::unexpected("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    auto ret = http->Read(data.get() + total_read, content_length - total_read);
                    if (!ret) {
                        return std::unexpected("Failed to download image: " + url);
                    }
                    if (*ret == 0) {
                        break;
                    }
                    total_read += *ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data.release(), total_read);
                if (!image->IsValid()) {
                    return std::unexpected("Downloaded image is invalid: " + url);
                }
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif  // CONFIG_LV_USE_SNAPSHOT
    }
#endif  // HAVE_LVGL

    // Assets download url (always registered — Settings storage works regardless of partition
    // layout)
    AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
                    PropertyList({Property("url", kPropertyTypeString)}),
                    [](const PropertyList& properties) -> ReturnValue {
                        auto url = properties["url"].value<std::string>();
                        Settings settings("assets", true);
                        settings.SetString("download_url", url);
                        return true;
                    });
}

namespace {

std::string BasicAuthHeaderValue(const std::string& username, const std::string& password) {
    const std::string credentials = username + ":" + password;
    size_t dlen = 0;
    mbedtls_base64_encode((unsigned char*)nullptr, 0, &dlen,
                           (const unsigned char*)credentials.data(), credentials.size());
    std::string encoded(dlen, '\0');
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char*)encoded.data(), encoded.size(), &olen,
                           (const unsigned char*)credentials.data(), credentials.size());
    // mbedtls 把终止 NUL 计入 olen，留在头部会污染请求
    encoded.resize(strlen(encoded.c_str()));
    return "Basic " + encoded;
}

}  // namespace

// base_url 统一按 A 语义处理：只取 scheme://host[:port]，路径交给 path 与 href
std::string McpServer::UrlOrigin(const std::string& url) {
    auto scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    size_t host_end = url.find('/', scheme + 3);
    return host_end == std::string::npos ? url : url.substr(0, host_end);
}

namespace {

bool IsAbsoluteUrl(const std::string& url) {
    return url.starts_with("http://") || url.starts_with("https://");
}

// path 视为服务器根路径：保证以单个 '/' 开头，并把非 ASCII、空格等字符百分号编码。
// 已有的 %XX 原样保留，避免对编码过的 WebDAV href 二次编码。
std::string NormalizePath(const std::string& path) {
    auto is_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    auto hex_digit = [](unsigned char c) -> char {
        return c < 10 ? static_cast<char>('0' + c) : static_cast<char>('A' + (c - 10));
    };
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(path[i]);
        if (c == '/') {
            if (!out.empty() && out.back() == '/') {
                continue;
            }
            out.push_back(c);
        } else if (c == '%' && i + 2 < path.size() && is_hex(path[i + 1]) && is_hex(path[i + 2])) {
            out.push_back(c);
            out.push_back(path[++i]);
            out.push_back(path[++i]);
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                   c == '~' || c == ':' || c == '@') {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex_digit(c >> 4));
            out.push_back(hex_digit(c & 0x0f));
        }
    }
    if (out.empty()) {
        return "/";
    }
    if (out.front() != '/') {
        out.insert(out.begin(), '/');
    }
    return out;
}

std::string JoinUrl(const std::string& origin, const std::string& path) {
    return origin + NormalizePath(path);
}

// WebDAV 的 href 是百分号编码的，显示前还原成可读文字
std::string UrlDecode(const std::string& in) {
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
            int hi = hex(in[i + 1]);
            int lo = hex(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

// 已配置的 WebDAV 音源列表（每个元素是源 JSON 字符串）
std::vector<std::string> LoadMusicSources() {
    Settings settings("music", true);
    CJsonUniquePtr sources(cJSON_Parse(settings.GetString("sources", "[]").c_str()));
    std::vector<std::string> result;
    if (sources == nullptr || !cJSON_IsArray(sources.get())) {
        return result;
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, sources.get()) {
        if (item == nullptr || !cJSON_IsObject(item)) {
            continue;
        }
        CJsonStringUniquePtr printed(cJSON_PrintUnformatted(item));
        if (printed != nullptr) {
            result.emplace_back(printed.get());
        }
    }
    return result;
}

bool MusicSourceAt(const std::vector<std::string>& sources, int index, std::string* out) {
    if (index < 0 || index >= static_cast<int>(sources.size())) {
        return false;
    }
    *out = sources[index];
    return true;
}

std::string GetDeviceIp() {
    CJsonUniquePtr json(cJSON_Parse(Board::GetInstance().GetBoardJson().c_str()));
    if (json == nullptr) {
        return "";
    }
    auto ip = cJSON_GetObjectItem(json.get(), "ip");
    if (ip == nullptr || !cJSON_IsString(ip) || strlen(ip->valuestring) == 0) {
        return "";
    }
    return ip->valuestring;
}

// 无音源时的引导话术：提示用户到设备网页端配置
std::string NoMusicSourceError() {
    std::string ip = GetDeviceIp();
    if (!ip.empty()) {
        return "未配置音乐音源。请在浏览器打开设备网页端 http://" + ip +
               "/ ，在「管理音源」中添加 WebDAV 音源后再试。";
    }
    return "未配置音乐音源。请在浏览器打开设备网页端，在「管理音源」中添加 WebDAV 音源后再试。";
}

bool HasExtension(const std::string& href, const char* const* exts, size_t count) {
    std::string lower = href;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (size_t i = 0; i < count; ++i) {
        size_t len = strlen(exts[i]);
        if (lower.size() > len && lower.compare(lower.size() - len, len, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

bool IsAudioHref(const std::string& href) {
    // aiff 无对应解码器（会被误当 MP3 解），故不列入
    static const char* kExts[] = {"mp3", "wav", "flac", "m4a", "aac", "ogg", "opus"};
    return HasExtension(href, kExts, std::size(kExts));
}

bool IsTxtHref(const std::string& href) {
    static const char* kExts[] = {".txt"};
    return HasExtension(href, kExts, std::size(kExts));
}

std::vector<std::string> ExtractXmlHrefs(
    const std::string& xml, int offset = 0, int limit = 0, int* total = nullptr,
    const std::function<bool(const std::string&)>& include = {}) {
    std::vector<std::string> hrefs;
    size_t pos = 0;
    int matched = 0;
    while (true) {
        // OpenList (and some servers) prefix element names with a namespace
        // prefix, e.g. "<D:response><D:href>/path</D:href>...", so accept both.
        size_t start = xml.find("<D:href>", pos);
        size_t start_len = 8;
        if (start == std::string::npos) {
            start = xml.find("<href>", pos);
            start_len = 7;
        }
        if (start == std::string::npos) {
            break;
        }
        size_t end = xml.find("</D:href>", start);
        size_t end_len = 9;
        if (end == std::string::npos) {
            end = xml.find("</href>", start);
            end_len = 7;
        }
        if (end == std::string::npos) {
            break;
        }
        std::string href = xml.substr(start + start_len, end - (start + start_len));
        size_t amp_pos = href.find("&");
        while (amp_pos != std::string::npos) {
            size_t semi = href.find(";", amp_pos);
            if (semi == std::string::npos) {
                break;
            }
            std::string ent = href.substr(amp_pos, semi - amp_pos + 1);
            std::string rep;
            if (ent == "&amp;") {
                rep = "&";
            } else if (ent == "&lt;") {
                rep = "<";
            } else if (ent == "&gt;") {
                rep = ">";
            } else if (ent == "&quot;") {
                rep = "\"";
            } else if (ent == "&apos;") {
                rep = "'";
            }
            if (!rep.empty()) {
                href.replace(amp_pos, ent.size(), rep);
            }
            amp_pos = href.find("&", amp_pos + rep.size());
        }
        if (include && !include(href)) {
            pos = end + end_len;
            continue;
        }
        if (total != nullptr) ++*total;
        if (limit <= 0 || (matched >= offset && matched < offset + limit)) {
            hrefs.push_back(href);
        }
        ++matched;
        pos = end + end_len;
    }
    return hrefs;
}

std::expected<std::string, std::string> WebdavList(
    const std::string& source, int offset, int limit, int* total,
    const std::function<bool(const std::string&)>& include) {
    CJsonUniquePtr source_json(cJSON_Parse(source.c_str()));
    if (source_json == nullptr || !cJSON_IsObject(source_json.get())) {
        return std::unexpected("Invalid source: expected a JSON object");
    }
    auto type = cJSON_GetObjectItem(source_json.get(), "type");
    if (type == nullptr || !cJSON_IsString(type) || strlen(type->valuestring) == 0) {
        return std::unexpected("Invalid source: missing type");
    }
    if (strcmp(type->valuestring, "webdav") != 0) {
        return std::unexpected("Invalid source: list only supported for webdav type");
    }
    auto base_url_item = cJSON_GetObjectItem(source_json.get(), "base_url");
    auto username_item = cJSON_GetObjectItem(source_json.get(), "username");
    auto password_item = cJSON_GetObjectItem(source_json.get(), "password");
    auto path_item = cJSON_GetObjectItem(source_json.get(), "path");
    if (base_url_item == nullptr || !cJSON_IsString(base_url_item) || strlen(base_url_item->valuestring) == 0) {
        return std::unexpected("Invalid source: webdav type requires base_url");
    }
    if (username_item == nullptr || !cJSON_IsString(username_item) || strlen(username_item->valuestring) == 0) {
        return std::unexpected("Invalid source: webdav type requires username");
    }
    if (password_item == nullptr || !cJSON_IsString(password_item) || strlen(password_item->valuestring) == 0) {
        return std::unexpected("Invalid source: webdav type requires password");
    }
    if (path_item == nullptr || !cJSON_IsString(path_item) || strlen(path_item->valuestring) == 0) {
        return std::unexpected("Invalid source: webdav type requires path");
    }
    std::string origin = McpServer::UrlOrigin(base_url_item->valuestring);
    std::string path = path_item->valuestring;
    std::string url = JoinUrl(origin, path);
    std::string username = username_item->valuestring;
    std::string password = password_item->valuestring;

    // 单次 PROPFIND：depth="infinity" 一次取回所有层级（含子目录），
    // 失败时由调用方回退到 "1" 保证兼容性（部分服务器不支持 infinity）。
    std::string body;
    std::string last_error;
    auto propfind = [&](const char* depth) -> bool {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        if (!http) {
            last_error = "Failed to create HTTP client";
            return false;
        }
        http->SetTimeout(15000);  // infinity 大目录响应较慢，放宽超时
        if (!username.empty() && !password.empty()) {
            http->SetHeader("Authorization", BasicAuthHeaderValue(username, password));
        }
        http->SetHeader("Accept", "application/xml");
        http->SetHeader("Content-Type", "application/xml");
        http->SetHeader("Depth", depth);
        // PROPFIND requires an XML request body describing the query.
        http->SetContent(std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                                     "<D:propfind xmlns:D=\"DAV:\">"
                                     "<D:prop><D:displayname/></D:prop>"
                                     "</D:propfind>"));
        if (auto opened = http->Open("PROPFIND", url); !opened) {
            last_error = "Failed to open URL: " + url + " (" + opened.error().ToString() + ")";
            http->Close();
            return false;
        }
        auto status = http->GetStatusCode();
        if (!status) {
            last_error = status.error().ToString();
            http->Close();
            return false;
        }
        if (*status < 200 || *status >= 300) {
            last_error = "WebDAV PROPFIND failed with status " + std::to_string(*status);
            http->Close();
            return false;
        }
        body = http->ReadAll();
        http->Close();
        last_error.clear();
        return true;
    };

    // 先试 infinity（含子目录）；失败则重试一次，仍失败则回退到单层 depth 1
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ok = propfind("infinity");
    }
    if (!ok) {
        ok = propfind("1");  // 兼容不支持 infinity 的服务器：退回旧行为
    }
    if (!ok) {
        return std::unexpected(last_error);
    }

    std::vector<std::string> hrefs = ExtractXmlHrefs(body, offset, limit, total, include);
    CJsonUniquePtr entries(cJSON_CreateArray());
    if (entries != nullptr) {
        for (const auto& href : hrefs) {
            cJSON_AddItemToArray(entries.get(), cJSON_CreateString(href.c_str()));
        }
    }
    CJsonStringUniquePtr printed(cJSON_PrintUnformatted(entries.get()));
    std::string json = printed ? std::string(printed.get()) : std::string("[]");
    return json;
}

}  // namespace

std::expected<std::string, std::string> McpServer::ListWebdav(const std::string& source) {
    return WebdavList(source, 0, 0, nullptr, {});
}

std::expected<std::string, std::string> McpServer::ListWebdavPage(
    const std::string& source, int offset, int limit, int* total, bool audio_only) {
    return WebdavList(source, offset, limit, total,
                      audio_only ? std::function<bool(const std::string&)>(IsAudioHref)
                                 : std::function<bool(const std::string&)>(IsTxtHref));
}

ToolResult McpServer::PlayMusic(const std::string& source) {
    CJsonUniquePtr source_json(cJSON_Parse(source.c_str()));
    if (source_json == nullptr || !cJSON_IsObject(source_json.get())) {
        return std::unexpected("Invalid source: expected a JSON object");
    }
    auto type = cJSON_GetObjectItem(source_json.get(), "type");
    if (type == nullptr || !cJSON_IsString(type) || strlen(type->valuestring) == 0) {
        return std::unexpected("Invalid source: missing type");
    }
    std::string audio_url;
    std::string username;
    std::string password;
    std::string title;
    if (strcmp(type->valuestring, "webdav") == 0) {
        auto base_url_item = cJSON_GetObjectItem(source_json.get(), "base_url");
        auto username_item = cJSON_GetObjectItem(source_json.get(), "username");
        auto password_item = cJSON_GetObjectItem(source_json.get(), "password");
        auto path_item = cJSON_GetObjectItem(source_json.get(), "path");
        if (base_url_item == nullptr || !cJSON_IsString(base_url_item) ||
            strlen(base_url_item->valuestring) == 0) {
            return std::unexpected("Invalid source: webdav type requires base_url");
        }
        if (username_item == nullptr || !cJSON_IsString(username_item) ||
            strlen(username_item->valuestring) == 0) {
            return std::unexpected("Invalid source: webdav type requires username");
        }
        if (password_item == nullptr || !cJSON_IsString(password_item) ||
            strlen(password_item->valuestring) == 0) {
            return std::unexpected("Invalid source: webdav type requires password");
        }
        if (path_item == nullptr || !cJSON_IsString(path_item) || strlen(path_item->valuestring) == 0) {
            return std::unexpected("Invalid source: webdav type requires path");
        }
        std::string base_url = base_url_item->valuestring;
        std::string path = path_item->valuestring;
        audio_url = JoinUrl(McpServer::UrlOrigin(base_url), path);
        username = username_item->valuestring;
        password = password_item->valuestring;
        title = NormalizePath(path);
        size_t last_slash = title.find_last_of('/');
        if (last_slash != std::string::npos) {
            title = UrlDecode(title.substr(last_slash + 1));
        }
    } else {
        return std::unexpected("Invalid source type: expected webdav");
    }

    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateIdle) {
        return std::unexpected("Device is not idle; cannot start music playback");
    }
    Application::MusicSource music_source;
    music_source.audio_url = std::move(audio_url);
    music_source.username = std::move(username);
    music_source.password = std::move(password);
    music_source.title = std::move(title);
    if (!app.StartMusic(music_source)) {
        return std::unexpected("Failed to start music playback");
    }
    return true;
}

__attribute__((noinline)) std::expected<std::vector<Application::MusicSource>, std::string>
McpServer::ListMusicTracks(const std::string& source, bool* truncated, int offset,
                           int limit, int* total) {
    int matched = 0;
    // Always filter by audio extension: the raw ListWebdav result also contains
    // directories and non-audio files, which would shift track indices.
    auto listing = ListWebdavPage(source, offset, limit, &matched, true);
    if (!listing) {
        return std::unexpected(listing.error());
    }
    constexpr size_t kMaxMusicTracks = 500;
    if (total != nullptr) *total = std::min(matched, static_cast<int>(kMaxMusicTracks));
    CJsonUniquePtr source_json(cJSON_Parse(source.c_str()));
    std::string origin;
    std::string username;
    std::string password;
    if (source_json != nullptr && cJSON_IsObject(source_json.get())) {
        auto base_url_item = cJSON_GetObjectItem(source_json.get(), "base_url");
        auto username_item = cJSON_GetObjectItem(source_json.get(), "username");
        auto password_item = cJSON_GetObjectItem(source_json.get(), "password");
        if (base_url_item != nullptr && cJSON_IsString(base_url_item)) {
            origin = McpServer::UrlOrigin(base_url_item->valuestring);
        }
        if (username_item != nullptr && cJSON_IsString(username_item)) {
            username = username_item->valuestring;
        }
        if (password_item != nullptr && cJSON_IsString(password_item)) {
            password = password_item->valuestring;
        }
    }

    std::vector<Application::MusicSource> tracks;
    CJsonUniquePtr entries(cJSON_Parse(listing->c_str()));
    if (entries != nullptr && cJSON_IsArray(entries.get())) {
        const int count = cJSON_GetArraySize(entries.get());
        for (int i = 0; i < count; ++i) {
            auto item = cJSON_GetArrayItem(entries.get(), i);
            if (item == nullptr || !cJSON_IsString(item)) {
                continue;
            }
            std::string href = item->valuestring;
            if (tracks.size() >= kMaxMusicTracks) {
                if (truncated != nullptr) {
                    *truncated = true;
                }
                break;
            }
            Application::MusicSource track;
            track.audio_url = IsAbsoluteUrl(href) ? href : JoinUrl(origin, href);
            track.username = username;
            track.password = password;
            size_t slash = href.find_last_of('/');
            track.title = UrlDecode(href.substr(slash + 1));
            tracks.push_back(std::move(track));
        }
    }
    return tracks;
}

__attribute__((noinline)) ToolResult McpServer::PlayMusicAll(const std::string& source, bool random) {
    // 未指定音源时回退到 NVS 里保存的选中音源
    std::string resolved_source = source;
    if (resolved_source.empty()) {
        auto sources = LoadMusicSources();
        if (sources.empty()) {
            return std::unexpected(NoMusicSourceError());
        }
        if (!MusicSourceAt(sources, Application::LoadSelectedSource(), &resolved_source)) {
            return std::unexpected("已保存的音源序号超出范围（共 " +
                                   std::to_string(sources.size()) + " 个）");
        }
    }
    // random=true 视为显式设置随机模式并持久化；否则读取 NVS 中保存的模式
    if (random) {
        Application::GetInstance().SetMusicMode(Application::MusicPlayMode::kShuffle);
    }
    bool truncated = false;
    auto tracks = ListMusicTracks(resolved_source, &truncated);
    if (!tracks) {
        return std::unexpected(tracks.error());
    }
    if (tracks->empty()) {
        return std::unexpected("No audio tracks found in the source");
    }

    auto& scheduler = Application::GetInstance().GetPlaybackScheduler();
    auto cmd = std::make_unique<PlaybackCommand>();
    cmd->type = PlaybackCommandType::PLAY;
    cmd->priority = PlaybackPriority::MUSIC;
    cmd->playlist = std::move(*tracks);
    cmd->source_index = -1;
    cmd->mode = Application::LoadPersistedMusicMode();
    cmd->track_index = 0;
    (void)truncated;

    return ToToolResult(scheduler.SubmitAndWait(std::move(cmd), 10000));
}

ToolResult McpServer::CountMusic(const std::string& source) {
    auto tracks = ListMusicTracks(source);
    if (!tracks) {
        return std::unexpected(tracks.error());
    }
    return static_cast<int>(tracks->size());
}

ToolResult McpServer::PlayMusicSourceAt(int index, bool random) {
    auto sources = LoadMusicSources();
    if (sources.empty()) {
        return std::unexpected(NoMusicSourceError());
    }
    std::string source;
    if (!MusicSourceAt(sources, index, &source)) {
        return std::unexpected("音源序号超出范围（共 " + std::to_string(sources.size()) + " 个）");
    }
    Application::SaveSelectedSource(index);  // 记住当前选中的音源
    if (random) {
        Application::GetInstance().SetMusicMode(Application::MusicPlayMode::kShuffle);
    }
    bool truncated = false;
    auto tracks = ListMusicTracks(source, &truncated);
    if (!tracks) {
        return std::unexpected(tracks.error());
    }
    if (tracks->empty()) {
        return std::unexpected("该音源没有音频文件");
    }

    auto& scheduler = Application::GetInstance().GetPlaybackScheduler();
    auto cmd = std::make_unique<PlaybackCommand>();
    cmd->type = PlaybackCommandType::PLAY;
    cmd->priority = PlaybackPriority::MUSIC;
    cmd->playlist = std::move(*tracks);
    cmd->source_index = index;
    cmd->mode = Application::LoadPersistedMusicMode();
    cmd->track_index = 0;
    (void)truncated;

    return ToToolResult(scheduler.SubmitAndWait(std::move(cmd), 10000));
}

ToolResult McpServer::SearchMusic(const std::string& keyword, int source_index) {
    if (keyword.empty()) {
        return std::unexpected("keyword 不能为空");
    }
    auto sources = LoadMusicSources();
    if (sources.empty()) {
        return std::unexpected(NoMusicSourceError());
    }
    std::string source;
    if (!MusicSourceAt(sources, source_index, &source)) {
        return std::unexpected("音源序号超出范围（共 " + std::to_string(sources.size()) + " 个）");
    }
    auto tracks = ListMusicTracks(source);
    if (!tracks) {
        return std::unexpected(tracks.error());
    }
    auto to_lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return text;
    };
    std::string lower_keyword = to_lower(keyword);
    CJsonUniquePtr matches(cJSON_CreateArray());
    for (size_t i = 0; i < tracks->size(); ++i) {
        std::string title = (*tracks)[i].title;
        if (to_lower(title).find(lower_keyword) == std::string::npos) {
            continue;
        }
        CJsonUniquePtr item(cJSON_CreateObject());
        cJSON_AddNumberToObject(item.get(), "index", static_cast<int>(i));
        cJSON_AddStringToObject(item.get(), "title", title.c_str());
        cJSON_AddItemToArray(matches.get(), item.get());
        item.release();
    }
    CJsonStringUniquePtr printed(cJSON_PrintUnformatted(matches.get()));
    return std::string(printed ? printed.get() : "[]");
}

ToolResult McpServer::PlayMusicTrackAt(int source_index, int track_index) {
    auto sources = LoadMusicSources();
    if (sources.empty()) {
        return std::unexpected(NoMusicSourceError());
    }
    std::string source;
    if (!MusicSourceAt(sources, source_index, &source)) {
        return std::unexpected("音源序号超出范围（共 " + std::to_string(sources.size()) + " 个）");
    }
    bool truncated = false;
    auto tracks = ListMusicTracks(source, &truncated);
    if (!tracks) {
        return std::unexpected(tracks.error());
    }
    if (track_index < 0 || track_index >= static_cast<int>(tracks->size())) {
        return std::unexpected("曲目序号超出范围（共 " + std::to_string(tracks->size()) + " 首）");
    }
    auto mode = Application::GetInstance().GetMusicStatus().mode;

    auto& scheduler = Application::GetInstance().GetPlaybackScheduler();
    auto cmd = std::make_unique<PlaybackCommand>();
    cmd->type = PlaybackCommandType::PLAY;
    cmd->priority = PlaybackPriority::MUSIC;
    cmd->playlist = std::move(*tracks);
    cmd->source_index = source_index;
    cmd->mode = mode;
    cmd->track_index = track_index;
    (void)truncated;

    return ToToolResult(scheduler.SubmitAndWait(std::move(cmd), 10000));
}

void McpServer::LogToolInventory() const {
    size_t user_only = 0;
    for (const auto& tool : tools_) {
        if (tool->user_only()) {
            ++user_only;
        }
    }
    ESP_LOGI(TAG, "%u tools registered: %u visible to the model, %u user-only",
             (unsigned)tools_.size(), (unsigned)(tools_.size() - user_only), (unsigned)user_only);
}

void McpServer::AddTool(std::unique_ptr<McpTool> tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [&tool](const auto& existing) {
            return existing->name() == tool->name();
        }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(std::move(tool));
}

void McpServer::AddTool(const std::string& name, const std::string& description,
                        const PropertyList& properties, ToolCallback callback) {
    AddTool(std::make_unique<McpTool>(name, description, properties, std::move(callback)));
}

void McpServer::AddTool(const std::string& name, const std::string& description,
                        const PropertyList& properties, ContextToolCallback callback) {
    AddTool(std::make_unique<McpTool>(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description,
                                const PropertyList& properties, ToolCallback callback) {
    auto tool = std::make_unique<McpTool>(name, description, properties, std::move(callback));
    tool->set_user_only(true);
    AddTool(std::move(tool));
}

namespace {
constexpr const char* kProtocolVersion = "io.modelcontextprotocol/protocolVersion";
constexpr const char* kClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
constexpr const char* kVisionExtension = "me.xiaozhi/vision";
constexpr const char* kModernVersion = "2026-07-28";
constexpr const char* kSupportedVersions = R"(["2026-07-28","2024-11-05"])";

std::string JsonString(const char* value) {
    CJsonUniquePtr string(cJSON_CreateString(value));
    CJsonStringUniquePtr encoded(string ? cJSON_PrintUnformatted(string.get()) : nullptr);
    return encoded ? std::string(encoded.get()) : std::string();
}

void AppendServerInfo(std::string& output) {
    output += "{\"name\":";
    output += JsonString(BOARD_NAME);
    output += ",\"version\":";
    output += JsonString(esp_app_get_description()->version);
    output += "}";
}

std::string ResultPrefix(const McpRequestContext& context) {
    std::string prefix = "{\"jsonrpc\":\"2.0\",\"id\":";
    prefix += context.id_json;
    prefix += ",\"result\":";
    if (context.modern) {
        prefix += "{\"resultType\":\"complete\",\"_meta\":{\"io.modelcontextprotocol/serverInfo\":";
        AppendServerInfo(prefix);
        prefix += "}}";
    }
    return prefix;
}

std::optional<McpRequestContext::VisionConfig> ReadVisionConfig(const cJSON* vision) {
    auto url = cJSON_GetObjectItemCaseSensitive(vision, "url");
    auto token = cJSON_GetObjectItemCaseSensitive(vision, "token");
    if (!cJSON_IsString(url)) {
        return std::nullopt;
    }
    return McpRequestContext::VisionConfig{url->valuestring,
                                           cJSON_IsString(token) ? token->valuestring : ""};
}

bool ValidVisionConfig(const McpRequestContext::VisionConfig& vision) {
    const auto& url = vision.url;
    const auto scheme_size = url.starts_with("https://")  ? 8u
                             : url.starts_with("http://") ? 7u
                                                          : 0u;
    if (scheme_size == 0 || url.size() <= scheme_size || url[scheme_size] == '/' ||
        url[scheme_size] == '?' || url[scheme_size] == '#') {
        return false;
    }
    return std::none_of(url.begin(), url.end(),
                        [](unsigned char c) { return std::isspace(c) || std::iscntrl(c); }) &&
           std::none_of(vision.token.begin(), vision.token.end(),
                        [](unsigned char c) { return c == '\r' || c == '\n'; });
}
}  // namespace

void McpServer::ParseMessage(const std::string& message, ResponseSender response_sender) {
    CJsonUniquePtr json(cJSON_ParseWithOpts(message.c_str(), nullptr, true));
    if (json == nullptr) {
        // Requests may contain credentials. Never log the original message.
        ReplyError({}, -32700, "Parse error", response_sender);
        return;
    }
    ParseMessage(json.get(), std::move(response_sender));
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto config = ReadVisionConfig(cJSON_GetObjectItem(capabilities, "vision"));
    if (config) {
        if (auto camera = Board::GetInstance().GetCamera()) {
            camera->SetExplainUrl(config->url, config->token);
        }
    }
}

void McpServer::ParseMessage(const cJSON* json, ResponseSender response_sender) {
    McpRequestContext context;
    auto id = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (cJSON_IsString(id)) {
        context.id_json = JsonString(id->valuestring);
    } else if (cJSON_IsNumber(id) && std::isfinite(id->valuedouble) &&
               std::trunc(id->valuedouble) == id->valuedouble &&
               std::abs(id->valuedouble) <= 9007199254740991.0) {
        // cJSON's floating formatter can round large safe integers. IDs must round-trip exactly.
        context.id_json = std::to_string(static_cast<int64_t>(id->valuedouble));
    }
    auto version = cJSON_GetObjectItemCaseSensitive(json, "jsonrpc");
    auto method = cJSON_GetObjectItemCaseSensitive(json, "method");
    if (!cJSON_IsObject(json) || !cJSON_IsString(version) ||
        strcmp(version->valuestring, "2.0") != 0 || !cJSON_IsString(method)) {
        ReplyError(context, -32600, "Invalid Request", response_sender);
        return;
    }
    if (id == nullptr) {
        return;  // Notifications never execute request-only tools or receive a response.
    }
    if (context.id_json.empty()) {
        ReplyError(context, -32600, "Invalid request ID", response_sender);
        return;
    }

    const std::string_view method_name(method->valuestring);
    if (method_name.starts_with("notifications/")) {
        return;  // Retain compatibility with notifications/initialized.
    }
    auto params = cJSON_GetObjectItemCaseSensitive(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ReplyError(context, -32602, "Invalid params", response_sender);
        return;
    }
    auto meta = cJSON_GetObjectItemCaseSensitive(params, "_meta");
    auto protocol_version = cJSON_GetObjectItemCaseSensitive(meta, kProtocolVersion);
    auto capabilities = cJSON_GetObjectItemCaseSensitive(meta, kClientCapabilities);
    context.modern = method_name == "server/discover" || protocol_version || capabilities ||
                     cJSON_GetObjectItemCaseSensitive(meta, "io.modelcontextprotocol/clientInfo") ||
                     cJSON_GetObjectItemCaseSensitive(meta, "io.modelcontextprotocol/logLevel");
    if ((meta && !cJSON_IsObject(meta)) ||
        (context.modern && (!cJSON_IsString(protocol_version) || !cJSON_IsObject(capabilities)))) {
        ReplyError(context, -32602, "Missing or invalid MCP request metadata", response_sender);
        return;
    }
    if (context.modern && strcmp(protocol_version->valuestring, kModernVersion) != 0) {
        std::string data = "{\"supported\":";
        data += kSupportedVersions;
        data += ",\"requested\":";
        data += JsonString(protocol_version->valuestring);
        data += "}";
        ReplyError(context, -32022, "Unsupported protocol version", response_sender, data);
        return;
    }
    auto extensions = cJSON_GetObjectItemCaseSensitive(capabilities, "extensions");
    auto client_info = cJSON_GetObjectItemCaseSensitive(meta, "io.modelcontextprotocol/clientInfo");
    if (context.modern &&
        ((extensions && !cJSON_IsObject(extensions)) ||
         (client_info &&
          (!cJSON_IsObject(client_info) ||
           !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(client_info, "name")) ||
           !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(client_info, "version")))))) {
        ReplyError(context, -32602, "Invalid MCP client capabilities or identity", response_sender);
        return;
    }

    if (method_name == "initialize" && !context.modern) {
        ParseCapabilities(cJSON_GetObjectItem(params, "capabilities"));
        std::string result =
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":";
        AppendServerInfo(result);
        result += "}";
        ReplyResult(context, result, response_sender);
    } else if (method_name == "server/discover") {
        std::string result = "{\"supportedVersions\":";
        result += kSupportedVersions;
        result += ",\"capabilities\":{\"tools\":{}";
        if (std::any_of(tools_.begin(), tools_.end(), [](const auto& tool) {
                return tool->name() == "self.camera.take_photo";
            })) {
            result += ",\"extensions\":{\"me.xiaozhi/vision\":{}}";
        }
        result += "},\"ttlMs\":60000,\"cacheScope\":\"private\"}";
        ReplyResult(context, result, response_sender);
    } else if (method_name == "tools/list") {
        auto cursor = cJSON_GetObjectItemCaseSensitive(params, "cursor");
        auto with_user_tools = cJSON_GetObjectItemCaseSensitive(params, "withUserTools");
        if (context.modern && ((cursor && !cJSON_IsString(cursor)) ||
                               (with_user_tools && !cJSON_IsBool(with_user_tools)))) {
            ReplyError(context, -32602, "Invalid tool-list parameters", response_sender);
            return;
        }
        GetToolsList(context, cJSON_IsString(cursor) ? cursor->valuestring : "",
                     cJSON_IsTrue(with_user_tools), response_sender);
    } else if (method_name == "tools/call") {
        auto name = cJSON_GetObjectItemCaseSensitive(params, "name");
        auto arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
        if (!cJSON_IsString(name) || (arguments && !cJSON_IsObject(arguments))) {
            ReplyError(context, -32602, "Invalid tool name or arguments", response_sender);
            return;
        }
        auto tool = std::find_if(tools_.begin(), tools_.end(), [&](const auto& item) {
            return item->name() == name->valuestring;
        });
        if (tool == tools_.end()) {
            std::string error = "Unknown tool: ";
            error += name->valuestring;
            ReplyError(context, -32602, error, response_sender);
            return;
        }
        if (context.modern && (*tool)->name() == "self.camera.take_photo") {
            auto vision = cJSON_GetObjectItemCaseSensitive(extensions, kVisionExtension);
            if (!vision) {
                ReplyError(context, -32021, "Missing required client capability", response_sender,
                           R"({"requiredCapabilities":{"extensions":{"me.xiaozhi/vision":{}}}})");
                return;
            }
            auto token = cJSON_GetObjectItemCaseSensitive(vision, "token");
            context.vision = ReadVisionConfig(vision);
            if (!cJSON_IsObject(vision) || !context.vision || (token && !cJSON_IsString(token)) ||
                !ValidVisionConfig(*context.vision)) {
                ReplyError(context, -32602, "Invalid vision configuration", response_sender);
                return;
            }
        }
        DoToolCall(std::move(context), tool->get(), arguments, std::move(response_sender));
    } else {
        ReplyError(context, -32601, "Method not implemented: " + std::string(method_name),
                   response_sender);
    }
}

void McpServer::SendResponse(const std::string& payload, const ResponseSender& response_sender) {
    if (response_sender) {
        response_sender(payload);
    } else {
        Application::GetInstance().SendMcpMessage(payload);
    }
}

void McpServer::ReplyResult(const McpRequestContext& context, const std::string& result,
                            const ResponseSender& response_sender) {
    auto payload = ResultPrefix(context);
    // Include the final brace so appending it cannot double a large response buffer.
    payload.reserve(payload.size() + result.size() + 1);
    if (context.modern) {
        // Merge known result objects into the envelope without parsing/copying a second JSON tree.
        payload.pop_back();
        if (result.size() > 2) {
            payload += ",";
        }
        payload.append(result, 1, std::string::npos);
    } else {
        payload += result;
    }
    payload += "}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(const McpRequestContext& context, int code, const std::string& message,
                           const ResponseSender& response_sender, const std::string& data) {
    auto encoded = JsonString(message.c_str());
    if (encoded.empty()) {
        return;
    }
    std::string payload;
    payload.reserve(context.id_json.size() + encoded.size() + data.size() + 80);
    payload += "{\"jsonrpc\":\"2.0\"";
    if (!context.id_json.empty()) {
        payload += ",\"id\":";
        payload += context.id_json;
    }
    payload += ",\"error\":{\"code\":";
    payload += std::to_string(code);
    payload += ",\"message\":";
    payload += encoded;
    if (!data.empty()) {
        payload += ",\"data\":";
        payload += data;
    }
    payload += "}}";
    SendResponse(payload, response_sender);
}

void McpServer::ReplyError(const McpRequestContext& context, const std::string& message,
                           const ResponseSender& response_sender) {
    ReplyError(context, -32603, message, response_sender);
}

void McpServer::GetToolsList(const McpRequestContext& context, const std::string& cursor,
                             bool list_user_only_tools, const ResponseSender& response_sender) {
    constexpr size_t kMaxPayloadSize = 8000;
    const size_t envelope_size = ResultPrefix(context).size() + 2;
    const std::string suffix = context.modern ? R"(],"ttlMs":60000,"cacheScope":"private")" : "]";
    auto it = tools_.begin();
    if (!cursor.empty()) {
        it = std::find_if(it, tools_.end(),
                          [&](const auto& tool) { return tool->name() == cursor; });
        if (it == tools_.end()) {
            ReplyError(context, -32602, "Invalid cursor", response_sender);
            return;
        }
    }
    auto visible = [&](auto position) {
        while (position != tools_.end() && !list_user_only_tools && (*position)->user_only()) {
            ++position;
        }
        return position;
    };
    it = visible(it);
    std::string json = "{\"tools\":[";
    std::string next_cursor;
    bool added = false;
    for (; it != tools_.end();) {
        auto next = visible(it + 1);
        auto tool_json = (*it)->to_json();
        const auto cursor_json = next == tools_.end() ? "" : JsonString((*next)->name().c_str());
        const size_t reserved = cursor_json.empty() ? 0 : cursor_json.size() + 14;
        if (tool_json.empty() ||
            json.size() + tool_json.size() + suffix.size() + reserved + envelope_size + 2 >
                kMaxPayloadSize) {
            if (!added) {
                ReplyError(context, "Tool definition exceeds payload limit", response_sender);
                return;
            }
            next_cursor = JsonString((*it)->name().c_str());
            break;
        }
        if (added) {
            json += ",";
        }
        json += tool_json;
        added = true;
        it = next;
    }
    json += suffix;
    if (!next_cursor.empty()) {
        json += ",\"nextCursor\":";
        json += next_cursor;
    }
    json += "}";
    ReplyResult(context, json, response_sender);
}

void McpServer::DoToolCall(McpRequestContext context, McpTool* tool, const cJSON* tool_arguments,
                           ResponseSender response_sender) {
    PropertyList arguments = tool->properties();
    for (auto& argument : arguments) {
        bool found = false;
        std::expected<void, std::string> validation;
        if (cJSON_IsObject(tool_arguments)) {
            auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
            if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                validation = argument.set_value<bool>(value->valueint == 1);
                found = true;
            } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value) &&
                       (!context.modern ||
                        (std::isfinite(value->valuedouble) &&
                         std::trunc(value->valuedouble) == value->valuedouble &&
                         value->valuedouble >= INT_MIN && value->valuedouble <= INT_MAX))) {
                validation = argument.set_value<int>(value->valueint);
                found = true;
            } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                validation = argument.set_value<std::string>(value->valuestring);
                found = true;
            } else if (value != nullptr) {
                ReplyError(context, -32602, "Invalid type for argument: " + argument.name(),
                           response_sender);
                return;
            }
        }
        if (found && !validation) {
            ReplyError(context, -32602, validation.error(), response_sender);
            return;
        }
        if (!argument.has_default_value() && !found) {
            ReplyError(context, -32602, "Missing required argument: " + argument.name(),
                       response_sender);
            return;
        }
    }
    // Log tool call entry (only tool name, args too verbose for serial)
    ESP_LOGI(TAG, "Tool call: %s", tool->name().c_str());

    // Reuse the existing application task. Context owns ID/vision after input JSON is destroyed.
    Application::GetInstance().Schedule([this, context = std::move(context), tool,
                                         arguments = std::move(arguments),
                                         response_sender = std::move(response_sender)]() {
        uint32_t start_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        auto result = tool->Call(arguments, context);
        uint32_t elapsed_ms = xTaskGetTickCount() * portTICK_PERIOD_MS - start_ms;
        if (!result) {
            ESP_LOGW(TAG, "Tool %s failed after %lu ms: %s", tool->name().c_str(),
                     static_cast<unsigned long>(elapsed_ms), result.error().c_str());
            if (context.modern) {
                std::string error = "{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":";
                error += JsonString(result.error().c_str());
                error += "}]}";
                ReplyResult(context, error, response_sender);
            } else {
                ReplyError(context, result.error(), response_sender);
            }
            return;
        }
        ESP_LOGD(TAG, "Tool %s success in %lu ms", tool->name().c_str(),
                 static_cast<unsigned long>(elapsed_ms));
        ReplyResult(context, *result, response_sender);
    });
}
