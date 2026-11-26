#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "cjson_utils.h"
#include "display.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "playback_scheduler.h"
#if CONFIG_USE_RADIO
#include "radio/radio_source.h"
#endif
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "weather/weather_service.h"
#include "ebook/ebook_player.h"
#include "websocket_protocol.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <limits>
#include <sys/time.h>
#include <esp_heap_caps.h>
#include <esp_random.h>

#define TAG "Application"

// Localized idle-screen text. Uses Kconfig language macros so generated
// lang_config.h stays untouched.
#if defined(CONFIG_LANGUAGE_ZH_CN) || defined(CONFIG_LANGUAGE_ZH_TW)
static const char* IdleGreeting(int hour) {
    if (hour >= 5 && hour < 11) return "早上好";
    if (hour >= 11 && hour < 13) return "中午好";
    if (hour >= 13 && hour < 18) return "下午好";
    return "晚上好";
}
static void IdleDateFormat(const struct tm* tm, char* out, size_t len) {
    static const char* weeks[] = {"日", "一", "二", "三", "四", "五", "六"};
    snprintf(out, len, "%d月%d日 周%s", tm->tm_mon + 1, tm->tm_mday, weeks[tm->tm_wday % 7]);
}
static const char* IdleWeatherHint(int code, float temp, bool valid) {
    if (!valid) return "天气获取中…";
    if (code >= 95) return "雷暴天气，尽量减少外出";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "出门记得带伞";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "道路湿滑，注意保暖";
    if (temp < 10) return "天气寒冷，注意添衣";
    if (temp > 30) return "天气炎热，注意补水";
    if (code == 0 && temp > 25) return "阳光明媚，适合外出";
    return "";
}
#else
static const char* IdleGreeting(int hour) {
    if (hour >= 5 && hour < 11) return "Good morning";
    if (hour >= 11 && hour < 13) return "Good noon";
    if (hour >= 13 && hour < 18) return "Good afternoon";
    return "Good evening";
}
static void IdleDateFormat(const struct tm* tm, char* out, size_t len) {
    strftime(out, len, "%b %d, %a", tm);
}
static const char* IdleWeatherHint(int code, float temp, bool valid) {
    if (!valid) return "Getting weather…";
    if (code >= 95) return "Thunderstorm, stay indoors";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "Take an umbrella";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "Slippery roads, bundle up";
    if (temp < 10) return "Cold, dress warmly";
    if (temp > 30) return "Hot, stay hydrated";
    if (code == 0 && temp > 25) return "Lovely day to go out";
    return "";
}
#endif

Application::Application() : notify_player_(audio_service_),
                              playback_scheduler_(std::make_unique<PlaybackScheduler>()) {
    event_group_ = xEventGroupCreate();
    playback_snapshot_ = static_cast<PlaybackSnapshot*>(
        heap_caps_malloc(sizeof(PlaybackSnapshot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

    // 电台缓冲看门狗：连接/缓冲超时后自动停台并恢复麦克风
    esp_timer_create_args_t wd_args = {
        .callback = RadioWatchdogCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "radio_watchdog",
        .skip_unhandled_events = true};
    esp_timer_create(&wd_args, &radio_watchdog_timer_);

    // 电台等待 TTS 兜底超时：AI 触发的起流若迟迟等不到 tts stop，到点强制起播
    esp_timer_create_args_t pending_args = {
        .callback = RadioPendingTimeoutCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "radio_pending",
        .skip_unhandled_events = true};
    esp_timer_create(&pending_args, &radio_pending_timer_);

    // 重连续播去抖 timer：网络恢复后延时 2s 再续播，等网络稳定
    esp_timer_create_args_t resume_args = {
        .callback = NetworkResumeTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "net_resume",
        .skip_unhandled_events = true};
    esp_timer_create(&resume_args, &network_resume_timer_);

    // 音乐播放中断续播重试 timer（指数退避）
    esp_timer_create_args_t retry_args = {
        .callback = MusicRetryTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "music_retry",
        .skip_unhandled_events = true};
    esp_timer_create(&retry_args, &music_retry_timer_);
}

Application::~Application() {
    notify_player_.Stop();
    if (radio_watchdog_timer_ != nullptr) {
        esp_timer_stop(radio_watchdog_timer_);
        esp_timer_delete(radio_watchdog_timer_);
        radio_watchdog_timer_ = nullptr;
    }
    if (radio_pending_timer_ != nullptr) {
        esp_timer_stop(radio_pending_timer_);
        esp_timer_delete(radio_pending_timer_);
        radio_pending_timer_ = nullptr;
    }
    if (network_resume_timer_ != nullptr) {
        esp_timer_stop(network_resume_timer_);
        esp_timer_delete(network_resume_timer_);
        network_resume_timer_ = nullptr;
    }
    if (music_retry_timer_ != nullptr) {
        esp_timer_stop(music_retry_timer_);
        esp_timer_delete(music_retry_timer_);
        music_retry_timer_ = nullptr;
    }
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (startup_wake_word_hint_timer_ != nullptr) {
        esp_timer_stop(startup_wake_word_hint_timer_);
        esp_timer_delete(startup_wake_word_hint_timer_);
    }
    vEventGroupDelete(event_group_);
    if (playback_snapshot_ != nullptr) {
        heap_caps_free(playback_snapshot_);
        playback_snapshot_ = nullptr;
    }
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);
    music_mode_ = LoadPersistedMusicMode();

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();
    ESP_LOGI(TAG, "After board/audio init");
    SystemInfo::PrintHeapStats();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    callbacks.on_playback_progress = [this](uint32_t playback_id, uint32_t media_position_ms) {
        music_position_ms_ = media_position_ms;
        notify_player_.OnPlaybackProgress(playback_id, media_position_ms);
    };
    audio_service_.SetCallbacks(callbacks);

    // Load local fonts, images and speech models before starting the network. A pending
    // update must finish first: these resources can reference the partition being rewritten.
    {
        Settings settings("assets", false);
        if (settings.GetString("download_url").empty()) {
            CheckAssetsVersion();
        }
    }

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();
    // Registration is complete here, so this is the only place the whole inventory exists.
    // It is logged because a server that accepts a bounded number of tools drops the rest
    // quietly: the only sign on this device was an alert at boot and a sad face, which is
    // easy to see and hard to connect to a tool list.
    mcp_server.LogToolInventory();

    // 初始化 PlaybackScheduler
    playback_scheduler_->Initialize(*this);

    // 保留 AI 对话日志，压低解码与网络组件的调试输出
    esp_log_level_set("MCP", ESP_LOG_INFO);
    esp_log_level_set("PROTO", ESP_LOG_INFO);
    esp_log_level_set("Application", ESP_LOG_WARN);
    // notify_player.cc 的 TAG 是 NotifyPlayer，之前的 "NOTIFY" 设置从未生效
    esp_log_level_set("NotifyPlayer", ESP_LOG_WARN);
    esp_log_level_set("WSControl", ESP_LOG_WARN);
    esp_log_level_set("HTTP", ESP_LOG_WARN);
    esp_log_level_set("HttpClient", ESP_LOG_WARN);
    esp_log_level_set("EspTcp", ESP_LOG_WARN);
    // GMF 内部逐任务/逐缓冲区日志只在排障时打开
    for (const char* tag : {"ESP_GMF_TASK", "ESP_GMF_HTTP", "ESP_GMF_BLOCK",
                            "ESP_GMF_PORT", "ESP_GMF_ASMP_DEC", "ESP_GMF_EVENT",
                            "ESP_GMF_POOL", "NEW_DATA_BUS", "AUD_SDEC_REG",
                            "ESP_ES_PARSER", "ESP_GMF_OBJ"}) {
        esp_log_level_set(tag, ESP_LOG_WARN);
    }

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED | MAIN_EVENT_STARTUP_HINT_TIMEOUT |
        MAIN_EVENT_PROTOCOL_PENDING;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
            }
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            if (audio_service_.IsPlaybackIdle()) {
                notify_player_.OnPlaybackDrained();
            }
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if ((bits & MAIN_EVENT_PROTOCOL_PENDING) && protocol_) {
            protocol_->ProcessPending();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    // Drop the remaining packets. Leaving them in the queue would
                    // stall the Opus codec task (it waits for queue space), which in
                    // turn deadlocks the whole audio input pipeline, as no new
                    // MAIN_EVENT_SEND_AUDIO event would ever be triggered again.
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            // Idle screen stays visible while radio/music plays (device is notifying).
            auto state = GetDeviceState();
            bool idle_screen = state == kDeviceStateIdle ||
                               (state == kDeviceStateNotifying &&
                                (radio_playing_ || music_active_)) ||
                               EbookPlayer::GetInstance().IsPlaying();
            if (idle_screen) {
                time_t now = time(nullptr);
                struct tm* tm_now = localtime(&now);
                if (tm_now != nullptr && tm_now->tm_year >= 2025 - 1900) {
                    Display::IdleScreenInfo idle;
                    snprintf(idle.greeting, sizeof(idle.greeting), "%s",
                             IdleGreeting(tm_now->tm_hour));
                    snprintf(idle.time, sizeof(idle.time), "%02d:%02d",
                             tm_now->tm_hour, tm_now->tm_min);
                    IdleDateFormat(tm_now, idle.date, sizeof(idle.date));

                    auto w = weather::GetLastWeather();
#if defined(CONFIG_LANGUAGE_ZH_CN) || defined(CONFIG_LANGUAGE_ZH_TW)
                    const char* high_low_fmt = "最高%.0f° / 最低%.0f°";
                    const char* details_fmt = "湿度%.0f%% · 风速%.0fkm/h";
#else
                    const char* high_low_fmt = "H %.0f° / L %.0f°";
                    const char* details_fmt = "Hum %.0f%% · Wind %.0fkm/h";
#endif
                    if (w.valid) {
                        snprintf(idle.temperature, sizeof(idle.temperature), "%.0f°C",
                                 w.temperature);
                        idle.temp_value = w.temperature;
                        idle.has_temp = true;
                        snprintf(idle.description, sizeof(idle.description), "%s",
                                 weather::Describe(w.weather_code).c_str());
                        if (w.has_daily) {
                            snprintf(idle.detail, sizeof(idle.detail), high_low_fmt,
                                     w.temp_max, w.temp_min);
                        }
                        if (w.has_details) {
                            snprintf(idle.detail2, sizeof(idle.detail2), details_fmt,
                                     w.humidity, w.wind_speed);
                        }
                        snprintf(idle.hint, sizeof(idle.hint), "%s",
                                 IdleWeatherHint(w.weather_code, w.temperature, true));
                        idle.weather_code = w.weather_code;
                    } else {
#if defined(CONFIG_LANGUAGE_ZH_CN) || defined(CONFIG_LANGUAGE_ZH_TW)
                        snprintf(idle.description, sizeof(idle.description), "天气获取中…");
#else
                        snprintf(idle.description, sizeof(idle.description), "Getting weather…");
#endif
                        snprintf(idle.hint, sizeof(idle.hint), "%s",
                                 IdleWeatherHint(0, 0, false));
                    }
                    const std::string city = weather::GetCity();
                    if (!city.empty()) {
                        snprintf(idle.city, sizeof(idle.city), "📍 %s", city.c_str());
                    } else {
#if defined(CONFIG_LANGUAGE_ZH_CN) || defined(CONFIG_LANGUAGE_ZH_TW)
                        snprintf(idle.city, sizeof(idle.city), "📍 定位中…");
#else
                        snprintf(idle.city, sizeof(idle.city), "📍 Locating…");
#endif
                    }
                    idle.icon_png = weather::GetIconPng();
                    const std::string title = GetPlaybackTitle();
                    snprintf(idle.title, sizeof(idle.title), "%s", title.c_str());
                    idle.valid = true;
                    display->SetIdleInfo(idle);
                }
            } else {
                display->SetIdleInfo(Display::IdleScreenInfo{});
            }

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
        if (bits & MAIN_EVENT_STARTUP_HINT_TIMEOUT) {
            HideStartupWakeWordHint();
        }
        ShowStartupWakeWordHint();
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    network_connected_ = true;

    // 断网期间有挂起的播放：延时去抖后恢复，避免刚拿到 IP 就连不上音源
    if (music_network_paused_ || radio_network_paused_) {
        if (network_resume_timer_ != nullptr) {
            esp_timer_start_once(network_resume_timer_, kNetworkResumeDebounceMs * 1000);
        }
    }

    auto state = GetDeviceState();
    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Initialize SNTP for accurate time (needed for daily shutdown & weather)
    // Moved here from NetworkEvent::Connected callback (lwip mbox not ready in callback)
    static bool sntp_started = false;
    if (!sntp_started) {
        sntp_started = true;
        esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2,
                                                                               ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.cloudflare.com"));
        sntp_config.sync_cb = [](struct timeval*) {
            time_t now = time(nullptr);
            struct tm tm_now = {};
            localtime_r(&now, &tm_now);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_now);
            ESP_LOGI(TAG, "SNTP synchronized, local time: %s", buf);
        };
        esp_netif_sntp_init(&sntp_config);
        ESP_LOGI(TAG, "SNTP initialized");
    }

    // Start weather updates once the network is ready (also sets the IP-based timezone)
    static bool weather_started = false;
    if (!weather_started) {
        weather_started = true;
        weather::Initialize("", nullptr);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    network_connected_ = false;
    // 取消待执行的重连续播去抖（短时间内再次断网）
    if (network_resume_timer_ != nullptr) {
        esp_timer_stop(network_resume_timer_);
    }

    auto state = GetDeviceState();
    // 音乐播放中：抓快照并暂停，保留会话与进度，待重连后续播（区别于用户主动暂停）
    if (state == kDeviceStateNotifying) {
        bool has_music = false;
        {
            std::lock_guard<std::mutex> lock(music_mutex_);
            has_music = music_active_ && !music_paused_;
        }
        if (has_music && PauseMusic()) {
            music_network_paused_ = true;
            ESP_LOGI(TAG, "Music paused on network loss, will resume on reconnect");
        } else {
            StopNotification();
        }
    }
    // 电台播放中：标记挂起并记下 URL，保留会话直到重连（不让其因重连次数耗尽而停台）
    if (radio_playing_ && radio_source_ != nullptr) {
        radio_network_url_ = radio_source_->GetCurrentUrl();
        if (!radio_network_url_.empty()) {
            radio_network_paused_ = true;
            ESP_LOGI(TAG, "Radio suspended on network loss, will reconnect on recovery: %s",
                     radio_network_url_.c_str());
        }
    }
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    // Protocol start may have already raised MAIN_EVENT_ERROR. Do not replace
    // that alert with the "ready" UI/sound — the main loop can process both
    // events back-to-back because the activation task is lower priority.
    const bool has_error = !last_error_message_.empty();
    if (!startup_completed_) {
        startup_completed_ = true;
        pending_startup_wake_word_hint_ = !has_error;
    }
    if (!has_error) {
        auto display = Board::GetInstance().GetDisplay();
        std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
    }

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::BALANCED);

    if (!has_error) {
        Schedule([this]() {
            // Play the success sound to indicate the device is ready
            audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
        });
    }
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        SetLowPowerIfIdle();
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    // "robot_2" is declared only by a board shipping its own pack: esp-vocat maps it to
    // neutral.eaf, and the rgb-matrix display special-cases the string. None of the shared
    // esp_emote_assets packs (320_240, 360_360, 1024_600) declares it, so on every board that
    // uses one this call fails, logs two errors and leaves whatever face was already up.
    // Fall back to an emotion all three packs do declare.
    if (!display->SetEmotion("robot_2")) {
        display->SetEmotion("neutral");
    }
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        auto check = ota_->CheckVersion();
        if (!check) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            const auto& err = check.error();
            char error_message[160];
            int error_message_length =
                snprintf(error_message, sizeof(error_message), "%s", err.ToString().c_str());
            if (error_message_length < 0 ||
                error_message_length >= static_cast<int>(sizeof(error_message))) {
                snprintf(error_message, sizeof(error_message), "%s", err.Message());
            }

            char buffer[320];
            int alert_message_length =
                snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED,
                         retry_delay, error_message);
            if (alert_message_length < 0 ||
                alert_message_length >= static_cast<int>(sizeof(buffer))) {
                snprintf(buffer, sizeof(buffer), "%s", err.Message());
            }
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, "
                     "resampling may cause distortion",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });

    protocol_->OnAudioChannelClosed([this]() {
        SetLowPowerIfIdle();
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "notify") == 0) {
            auto audio_url = cJSON_GetObjectItem(root, "audio_url");
            if (!cJSON_IsString(audio_url) || audio_url->valuestring[0] == '\0') {
                ESP_LOGW(TAG, "Notify message requires audio_url");
                return;
            }

            std::vector<NotifySubtitle> subtitles;
            auto subtitles_json = cJSON_GetObjectItem(root, "subtitles");
            if (subtitles_json != nullptr && !cJSON_IsArray(subtitles_json)) {
                ESP_LOGW(TAG, "Notify subtitles must be an array");
                return;
            }
            if (cJSON_IsArray(subtitles_json)) {
                cJSON* item = nullptr;
                cJSON_ArrayForEach (item, subtitles_json) {
                    auto start_ms = cJSON_GetObjectItem(item, "start_ms");
                    auto text = cJSON_GetObjectItem(item, "text");
                    if (!cJSON_IsNumber(start_ms) || start_ms->valuedouble < 0 ||
                        start_ms->valuedouble > std::numeric_limits<uint32_t>::max() ||
                        !cJSON_IsString(text)) {
                        ESP_LOGW(TAG, "Ignoring invalid notify subtitle");
                        continue;
                    }
                    subtitles.push_back({.start_ms = static_cast<uint32_t>(start_ms->valuedouble),
                                         .text = text->valuestring});
                }
            }

            Schedule([this, url = std::string(audio_url->valuestring),
                      subtitles = std::move(subtitles)]() mutable {
                StartNotification(std::move(url), std::move(subtitles));
            });
        } else if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                    // 本轮 TTS 结束：解除电台的等待标记并停掉兜底 timer
                    if (pending_radio_wait_tts_) {
                        if (radio_pending_timer_ != nullptr) {
                            esp_timer_stop(radio_pending_timer_);
                        }
                        pending_radio_wait_tts_ = false;
                    }
                    MaybeStartPendingRadio();
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([display, message = std::string(text->valuestring),
                              glyphs = std::move(glyphs), bpp]() {
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            } else if (strcmp(state->valuestring, "sentence_end") == 0) {
                // TTS 句子播放结束，通知 AI 侧
                SendTtsFinished();
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            CJsonStringUniquePtr root_json(cJSON_PrintUnformatted(root));
            ESP_LOGI(TAG, "Received custom message: %s", root_json ? root_json.get() : "");
            if (cJSON_IsObject(payload)) {
                CJsonStringUniquePtr payload_json(cJSON_PrintUnformatted(payload));
                if (payload_json) {
                    Schedule([this, display, payload_str = std::string(payload_json.get())]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
                }
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    last_error_message_.clear();
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
        state = kDeviceStateIdle;
    }

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateNotifying) {
        StopNotification();
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateNotifying) {
        StopNotification();
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word) {
    // Must run in the main task with the device in idle state
    audio_service_.EncodeWakeWord();

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word]() { ContinueWakeWordInvoke(wake_word); });
        return;
    }
    // Channel already opened, continue directly
    ContinueWakeWordInvoke(wake_word);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    // Set flag to play popup sound after state changes to listening
    // (PlaySound here would be cleared by ResetDecoder in EnableVoiceProcessing)
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            // Keep a just-raised network error visible. SetDeviceState(idle)
            // queues STATE_CHANGED after Alert(), and the idle handler would
            // otherwise wipe the status, emotion, and chat message.
            if (last_error_message_.empty()) {
                display->SetStatus(Lang::Strings::STANDBY);
                display->ClearChatMessages();  // Clear messages first
                display->SetEmotion(
                    "neutral");  // Then set emotion (wechat mode checks child count)
            }
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(!radio_playing_);
            MaybeStartPendingRadio();
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for the playback queue to drain before enabling
                // voice processing. This prevents audio truncation when STOP arrives
                // late due to network jitter. Instead of blocking the main loop here,
                // defer the start until MAIN_EVENT_PLAYBACK_DRAINED arrives.
                if (listening_mode_ == kListeningModeAutoStop && !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateNotifying:
            display->SetStatus(Lang::Strings::SPEAKING);
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
    if (new_state != kDeviceStateIdle &&
        !(new_state == kDeviceStateNotifying && (radio_playing_ || music_active_)) &&
        !EbookPlayer::GetInstance().IsPlaying()) {
        display->SetIdleInfo(Display::IdleScreenInfo{});
    }
}

void Application::ShowStartupWakeWordHint() {
    // A coalesced activation/state event can queue another idle UI cleanup.
    // Wait for it before adding the hint, or ClearChatMessages() would erase it.
    if (!pending_startup_wake_word_hint_ ||
        (xEventGroupGetBits(event_group_) & MAIN_EVENT_STATE_CHANGED)) {
        return;
    }
    pending_startup_wake_word_hint_ = false;
    const auto state = GetDeviceState();
    const bool wake_word_running = audio_service_.IsWakeWordRunning();
    if (state != kDeviceStateIdle || !last_error_message_.empty() || !wake_word_running) {
        ESP_LOGI(TAG, "Startup wake word hint skipped (state=%d, wake word running=%d, error=%d)",
                 static_cast<int>(state), wake_word_running, !last_error_message_.empty());
        return;
    }

    const auto wake_words = audio_service_.GetWakeWords();
    std::string words;
    for (auto it = wake_words.begin(); it != wake_words.end(); ++it) {
        if (it->empty() || std::find(wake_words.begin(), it, *it) != it) {
            continue;
        }
        if (!words.empty()) {
            words += " / ";
        }
        words += *it;
    }
    if (words.empty()) {
        ESP_LOGW(TAG, "Startup wake word hint skipped: no available wake words");
        return;
    }
    std::string message = Lang::Strings::WAKE_WORD_HINT;
    const auto placeholder = message.find("%s");
    if (placeholder == std::string::npos) {
        ESP_LOGE(TAG, "Startup wake word hint skipped: invalid message template");
        return;
    }
    message.replace(placeholder, 2, words);
    if (startup_wake_word_hint_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback =
                [](void* arg) {
                    auto app = static_cast<Application*>(arg);
                    xEventGroupSetBits(app->event_group_, MAIN_EVENT_STARTUP_HINT_TIMEOUT);
                },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "startup_hint",
            .skip_unhandled_events = false,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &startup_wake_word_hint_timer_));
    }
    ESP_LOGI(TAG, "Startup wake word hint: %s", message.c_str());
    Board::GetInstance().GetDisplay()->SetChatMessage("assistant", message.c_str());
    startup_wake_word_hint_message_ = std::move(message);
    ESP_ERROR_CHECK(esp_timer_start_once(startup_wake_word_hint_timer_, 5000 * 1000));
}

void Application::HideStartupWakeWordHint() {
    if (startup_wake_word_hint_message_.empty()) {
        return;
    }
    // A conversation or alert may have replaced the hint while its timer was running.
    Board::GetInstance().GetDisplay()->RemoveChatMessage("assistant",
                                                         startup_wake_word_hint_message_.c_str());
    startup_wake_word_hint_message_.clear();
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

    // Send the start listening command
    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

    // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::InterruptSpeakingForPlayback() {
    auto state = GetDeviceState();
    if (state != kDeviceStateSpeaking && state != kDeviceStateListening &&
        state != kDeviceStateConnecting) {
        return;
    }
    AbortSpeaking(kAbortReasonNone);
    audio_service_.ResetDecoder();          // 立刻丢掉本地排队的 TTS 音频
    while (audio_service_.PopPacketFromSendQueue()) {
    }
    SetDeviceState(kDeviceStateIdle);       // Speaking/Listening/Connecting -> Idle 均合法
}

bool Application::StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles,
                                   const std::string& username, const std::string& password,
                                   const std::string& title, uint64_t start_byte_pos,
                                   uint32_t start_pos_ms, uint32_t start_duration_ms) {
    (void)title;  // track name is shown in the top bar via GetPlaybackTitle()
    // 音乐与电台共用输出设备：先停电台，避免两路 PCM 混在一起
    if (radio_playing_) {
        StopRadio();
    }
    // AI 说话期间工具调用会落到 Speaking 状态，先打断再启动播放
    InterruptSpeakingForPlayback();
    if (GetDeviceState() != kDeviceStateIdle || notify_player_.IsBusy()) {
        ESP_LOGW(TAG, "Ignoring notify message while device is busy");
        return false;
    }

    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
    audio_service_.ReleaseWakeWordResources();
    while (audio_service_.PopPacketFromSendQueue()) {
        // Discard microphone audio left over from a previous conversation.
    }

    if (!SetDeviceState(kDeviceStateNotifying)) {
        SetLowPowerIfIdle();
        return false;
    }

    audio_service_.ResetDecoder();
    uint32_t playback_id = ++notification_playback_id_;
    if (playback_id == 0) {
        playback_id = ++notification_playback_id_;
    }
    audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);

    bool started = notify_player_.Start(
        std::move(audio_url), std::move(subtitles), playback_id,
        [this](uint32_t id, const std::string& text) {
            Schedule([this, id, text]() {
                if (GetDeviceState() == kDeviceStateNotifying && notification_playback_id_ == id) {
                    Board::GetInstance().GetDisplay()->SetChatMessage("assistant", text.c_str());
                }
            });
        },
        [this](uint32_t id, bool success) {
            Schedule([this, id, success]() { HandleNotificationFinished(id, success); });
        },
        username, password, start_byte_pos, start_pos_ms, start_duration_ms);

    if (!started) {
        ESP_LOGE(TAG, "Failed to start notification playback");
        StopNotification();
        return false;
    }
    // 外部播放（音乐）开始：降低屏幕背光耗电；AI 语音不参与此减耗
    SetBacklightForPlayback(music_active_);
    return true;
}

void Application::StopNotification(bool force) {
    if (!force && music_paused_) {
        // 暂停期间协议重置等内部流程不应结束会话；底层已停流，保留快照与 Notifying 状态
        notify_player_.Stop();
        return;
    }
    notify_player_.Stop();
    audio_service_.ResetDecoder();
    auto& board = Board::GetInstance();
    board.GetDisplay()->SetChatMessage("assistant", "");
    SetLowPowerIfIdle();
    SetBacklightForPlayback(false);
    if (GetDeviceState() == kDeviceStateNotifying) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ClearMusicSessionLocked() {
    music_playlist_.clear();
    music_order_.clear();
    music_cursor_ = 0;
    music_source_index_ = -1;
    music_active_ = false;
    music_paused_ = false;
    music_single_ = false;
    music_fail_streak_ = 0;
    music_truncated_ = false;
    music_position_ms_ = 0;
    music_network_paused_ = false;
    music_resume_retries_ = 0;
    if (playback_snapshot_ != nullptr) {
        playback_snapshot_->valid = false;
    }
}

bool Application::StartMusic(const MusicSource& source) {
    if (radio_playing_ || radio_switching_) {
        uint32_t seq = BeginSourceSwitch();
        auto copy = source;
        StopRadio([this, copy, seq]() {
            if (IsCurrentSourceSwitch(seq)) StartMusic(copy);
        });
        return true;
    }
    uint32_t seq = BeginSourceSwitch();
    (void)seq;
    StopNotification(true);
    StopEbook();
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        ClearMusicSessionLocked();
        music_playlist_.push_back(source);
        music_order_.push_back(0);
        music_single_ = true;
        music_active_ = true;
        music_paused_ = false;
    }
    if (!StartNotification(source.audio_url, {}, source.username, source.password, source.title)) {
        std::lock_guard<std::mutex> lock(music_mutex_);
        ClearMusicSessionLocked();
        return false;
    }
    return true;
}

bool Application::PlayMusicPlaylist(std::vector<MusicSource> tracks, int source_index,
                                    MusicPlayMode mode, int start_index, bool truncated) {
    if (tracks.empty()) {
        return false;
    }
    if (radio_playing_ || radio_switching_) {
        uint32_t seq = BeginSourceSwitch();
        auto copy = tracks;
        StopRadio([this, copy, seq, source_index, mode, start_index, truncated]() {
            if (IsCurrentSourceSwitch(seq)) {
                PlayMusicPlaylist(copy, source_index, mode, start_index, truncated);
            }
        });
        return true;
    }
    uint32_t seq = BeginSourceSwitch();
    (void)seq;
    StopEbook();
    if (start_index < 0 || start_index >= static_cast<int>(tracks.size())) {
        start_index = 0;
    }
    StopNotification(true);
    MusicSource first;
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        ClearMusicSessionLocked();
        music_playlist_ = std::move(tracks);
        music_source_index_ = source_index;
        music_mode_ = mode;
        music_active_ = true;
        music_paused_ = false;
        music_truncated_ = truncated;
        music_order_.resize(music_playlist_.size());
        for (size_t i = 0; i < music_order_.size(); ++i) {
            music_order_[i] = i;
        }
        if (mode == MusicPlayMode::kShuffle) {
            for (size_t i = music_order_.size(); i > 1; --i) {
                size_t j = esp_random() % i;
                std::swap(music_order_[i - 1], music_order_[j]);
            }
        }
        // 从指定曲目开始，保持其余曲目的相对顺序
        auto begin = std::find(music_order_.begin(), music_order_.end(),
                               static_cast<size_t>(start_index));
        if (begin != music_order_.end()) {
            std::rotate(music_order_.begin(), begin, music_order_.end());
        }
        music_cursor_ = 0;
        first = music_playlist_[music_order_[0]];
    }
    if (!StartNotification(first.audio_url, {}, first.username, first.password, first.title)) {
        std::lock_guard<std::mutex> lock(music_mutex_);
        ClearMusicSessionLocked();
        return false;
    }
    return true;
}

bool Application::PlayMusicTrackAt(int index) {
    MusicSource source;
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (!music_active_ || music_playlist_.empty() || index < 0 ||
            index >= static_cast<int>(music_playlist_.size())) {
            return false;
        }
        auto it = std::find(music_order_.begin(), music_order_.end(), static_cast<size_t>(index));
        if (it == music_order_.end()) {
            return false;
        }
        music_cursor_ = static_cast<size_t>(it - music_order_.begin());
        music_single_ = false;
        music_fail_streak_ = 0;
        source = music_playlist_[index];
    }
    StopNotification(true);
    if (!StartNotification(source.audio_url, {}, source.username, source.password, source.title)) {
        std::lock_guard<std::mutex> lock(music_mutex_);
        ClearMusicSessionLocked();
        return false;
    }
    return true;
}

bool Application::PlayNextMusic() {
    MusicSource next;
    bool has_next = false;
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (!music_active_ || music_playlist_.empty()) {
            return false;
        }
        if (music_cursor_ + 1 < music_order_.size()) {
            ++music_cursor_;
            music_fail_streak_ = 0;
            next = music_playlist_[music_order_[music_cursor_]];
            has_next = true;
        } else {
            ClearMusicSessionLocked();
        }
    }
    StopNotification(true);
    if (has_next) {
        if (!StartNotification(next.audio_url, {}, next.username, next.password, next.title)) {
            std::lock_guard<std::mutex> lock(music_mutex_);
            ClearMusicSessionLocked();
            return false;
        }
        return true;
    }
    return false;
}

bool Application::PlayPrevMusic() {
    MusicSource prev;
    bool has_prev = false;
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (!music_active_ || music_playlist_.empty()) {
            return false;
        }
        if (music_cursor_ > 0) {
            --music_cursor_;
        }
        music_fail_streak_ = 0;
        prev = music_playlist_[music_order_[music_cursor_]];
        has_prev = true;
    }
    StopNotification(true);
    if (has_prev) {
        if (!StartNotification(prev.audio_url, {}, prev.username, prev.password, prev.title)) {
            std::lock_guard<std::mutex> lock(music_mutex_);
            ClearMusicSessionLocked();
            return false;
        }
        return true;
    }
    return false;
}

bool Application::CapturePlaybackSnapshot() {
    NotifyPlayer::PlaybackInfo info;
    bool have_info = notify_player_.GetPlaybackInfo(info);
    std::lock_guard<std::mutex> lock(music_mutex_);
    if (!music_active_ || playback_snapshot_ == nullptr) {
        return false;
    }
    // 播放中断回调里 active_ 已清，GetPlaybackInfo 会失败；此时用 NotifyPlayer
    // 持续记录的原子值兜底，只要有真实 IO 字节位置就能断点续播。
    uint64_t byte_pos;
    uint64_t file_size;
    uint32_t position_ms;
    uint32_t duration;
    uint32_t playback_id;
    if (have_info) {
        byte_pos = info.byte_pos;
        file_size = info.file_size;
        position_ms = info.position_ms;
        duration = info.duration_ms;
        playback_id = info.playback_id;
    } else {
        byte_pos = notify_player_.GetLastBytePos();
        file_size = notify_player_.GetLastFileSize();
        position_ms = notify_player_.GetLastPositionMs();
        duration = notify_player_.GetLastDurationMs();
        playback_id = notification_playback_id_;
        if (byte_pos == 0) {
            return false;  // 从未读到音频数据，无法断点续播
        }
    }
    auto& s = *playback_snapshot_;
    if (duration == 0 && byte_pos > 0 && position_ms > 0 && file_size > byte_pos) {
        duration = static_cast<uint32_t>(static_cast<double>(file_size) * position_ms / byte_pos);
    }
    s.token = playback_id;
    s.source_index = music_source_index_;
    s.cursor = static_cast<int>(music_cursor_);
    s.position_ms = position_ms;
    s.duration_ms = duration;
    s.byte_pos = byte_pos;      // 暂停/中断瞬间的真实 IO 位置
    s.file_size = file_size;
    s.mode = static_cast<uint8_t>(music_mode_);
    s.restart_from_start = false;
    s.valid = true;
    ESP_LOGD(TAG, "Capture snapshot: pos=%u dur=%u byte=%llu size=%llu",
             s.position_ms, s.duration_ms, static_cast<unsigned long long>(s.byte_pos),
             static_cast<unsigned long long>(s.file_size));
    return true;
}

void Application::UpdateSnapshotPositionLocked(uint32_t time_ms) {
    auto& s = *playback_snapshot_;
    uint32_t target = time_ms;
    if (s.duration_ms > 0 && target > s.duration_ms) {
        target = s.duration_ms;
    }
    if (s.duration_ms > 0 && s.file_size > 0) {
        s.byte_pos = static_cast<uint64_t>(static_cast<double>(target) / s.duration_ms * s.file_size);
    } else if (s.position_ms > 0 && s.byte_pos > 0) {
        // 时长未知时用已保存的真实 byte_pos 按比例推算
        s.byte_pos = static_cast<uint64_t>(static_cast<double>(target) / s.position_ms * s.byte_pos);
    }
    s.position_ms = target;
    music_position_ms_ = static_cast<int>(target);
}

bool Application::RestartFromSnapshot() {
    PlaybackSnapshot snap;
    MusicSource track;
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (!music_active_ || playback_snapshot_ == nullptr || !playback_snapshot_->valid) {
            return false;
        }
        snap = *playback_snapshot_;
        if (snap.cursor < 0 || static_cast<size_t>(snap.cursor) >= music_order_.size()) {
            return false;
        }
        size_t index = music_order_[snap.cursor];
        if (index >= music_playlist_.size()) {
            return false;
        }
        track = music_playlist_[index];
    }
    // notifying -> idle 是合法转换，StartNotification 会重新进入 notifying
    auto previous_state = GetDeviceState();
    if (previous_state == kDeviceStateNotifying) {
        SetDeviceState(kDeviceStateIdle);
    }
    uint64_t start_byte = snap.restart_from_start ? 0 : snap.byte_pos;
    uint32_t start_pos = snap.restart_from_start ? 0 : snap.position_ms;
    ESP_LOGD(TAG, "Restart playback at %u ms, byte_pos=%llu, duration=%u ms, from_start=%d",
             start_pos, static_cast<unsigned long long>(start_byte), snap.duration_ms,
             snap.restart_from_start);
    bool ok = StartNotification(track.audio_url, {}, track.username, track.password, track.title,
                                start_byte, start_pos, snap.duration_ms);
    if (ok) {
        std::lock_guard<std::mutex> lock(music_mutex_);
        playback_snapshot_->valid = false;
        music_position_ms_ = static_cast<int>(snap.position_ms);
    } else if (previous_state == kDeviceStateNotifying) {
        // 启动失败时回到原状态，避免网页看到 Idle 但会话仍标记为暂停
        SetDeviceState(kDeviceStateNotifying);
    }
    return ok;
}

bool Application::PauseMusic() {
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (!music_active_ || music_paused_) {
            return false;
        }
    }
    if (!CapturePlaybackSnapshot()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (!music_active_ || music_paused_) {
            return false;
        }
        music_paused_ = true;
        music_position_ms_ = static_cast<int>(playback_snapshot_->position_ms);
    }
    notify_player_.Stop();
    SetBacklightForPlayback(false);  // 暂停：回到正常亮度
    return true;
}

bool Application::ResumeMusic() {
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (!music_active_ || !music_paused_) {
            return false;
        }
    }
    bool ok = RestartFromSnapshot();
    if (ok) {
        std::lock_guard<std::mutex> lock(music_mutex_);
        music_paused_ = false;
    }
    return ok;
}

bool Application::SeekMusic(uint32_t time_ms) {
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (!music_active_ || music_playlist_.empty() || music_cursor_ >= music_order_.size()) {
            return false;
        }
        if (music_paused_ && playback_snapshot_ != nullptr && playback_snapshot_->valid) {
            UpdateSnapshotPositionLocked(time_ms);
            return true;
        }
    }
    // 播放中：与暂停共用同一份 PSRAM 快照，按同一路径重启
    if (!CapturePlaybackSnapshot()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        UpdateSnapshotPositionLocked(time_ms);
    }
    notify_player_.Stop();  // 只停流，保留会话与设备状态
    return RestartFromSnapshot();
}

void Application::StopMusicPlayback() {
    if (music_retry_timer_ != nullptr) {
        esp_timer_stop(music_retry_timer_);
    }
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        ClearMusicSessionLocked();
    }
    StopNotification(true);
}

bool Application::PlayEbook(const std::string& url, const std::string& username,
                            const std::string& password) {
    uint32_t seq = BeginSourceSwitch();
    // 输出只有一个：先停掉音乐/电台，避免两路 PCM 争抢 codec
    if (GetMusicStatus().active) StopMusicPlayback();
    if (radio_playing_ || radio_switching_) {
        StopRadio([this, url, username, password, seq]() {
            if (IsCurrentSourceSwitch(seq)) StartEbookNow(url, username, password);
        });
        return true;
    }
    return StartEbookNow(url, username, password);
}

bool Application::StartEbookNow(const std::string& url, const std::string& username,
                                const std::string& password) {
    return EbookPlayer::GetInstance().Play(url, username, password);
}
void Application::StopEbook() { EbookPlayer::GetInstance().Stop(); }
bool Application::EbookPause() { return EbookPlayer::GetInstance().Pause(); }
bool Application::EbookResume() { return EbookPlayer::GetInstance().Resume(); }
bool Application::EbookNext() { return EbookPlayer::GetInstance().Next(); }
bool Application::EbookReplay() { return EbookPlayer::GetInstance().Replay(); }
bool Application::EbookSeek(int percent) { return EbookPlayer::GetInstance().Seek(percent); }
void Application::TestEbookTts(const std::string& text, const std::string& voice, int speed) {
    EbookPlayer::GetInstance().PlayTestAsync(text, voice, speed);
}

std::string Application::GetPlaybackTitle() const {
    if (EbookPlayer::GetInstance().IsPlaying()) {
        auto st = EbookPlayer::GetInstance().GetStatus();
        if (!st.book_name.empty()) return st.book_name;
    }
    if (radio_playing_ && radio_source_ != nullptr) {
        auto station = radio_source_->GetStationManager().FindByUrl(radio_source_->GetCurrentUrl());
        if (station.has_value()) return station->name;
    }
    if (music_active_) {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (music_cursor_ < music_order_.size()) {
            size_t index = music_order_[music_cursor_];
            if (index < music_playlist_.size()) return music_playlist_[index].title;
        }
    }
    return "";
}

bool Application::IsExternalPlaybackActive() const {
    return radio_playing_ || radio_switching_ || GetMusicStatus().active ||
           EbookPlayer::GetInstance().IsPlaying();
}

void Application::SetLowPowerIfIdle() {
    if (!IsExternalPlaybackActive()) {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    }
}

void Application::SetRadioPlaying(bool playing) {
    radio_playing_ = playing;
    audio_service_.SetExternalPlaybackActive(playing);
    if (playing) {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    } else {
        SetLowPowerIfIdle();
    }
    SetBacklightForPlayback(playing);
    // 电台播放期间关闭唤醒词，避免扬声器声音回灌；停止后恢复
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.EnableWakeWordDetection(!playing && audio_service_.IsAfeWakeWord());
}

void Application::SetBacklightForPlayback(bool playing) {
    Backlight* backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) {
        return;
    }
    if (playing) {
        // 外部播放（电台/音乐）期间降低屏幕背光耗电；恢复时回到用户设置亮度
        backlight->SetBrightness(kPlaybackBacklightDim);
    } else {
        backlight->RestoreBrightness();
    }
}

void Application::StartRadio(const std::string& url, bool wait_for_tts) {
    if (url.empty()) return;
    StopEbook();
    // 与音乐互斥：先停掉音乐会话，避免两路 PCM 争抢 codec
    if (GetMusicStatus().active) StopMusicPlayback();

    if (radio_playing_ || radio_switching_) {
        // 切台必须等旧 GMF 任务真正停完再起新的，否则两代 HLS IO 会抢
        // lifecycle_mutex_，旧流读超时会污染新流。stop 放到一次性任务里，
        // 完成后回主任务继续起流，避免卡住主事件循环。
        uint32_t seq = BeginSourceSwitch();
        StopRadio([this, url, wait_for_tts, seq]() {
            if (IsCurrentSourceSwitch(seq)) StartRadioAfterStop(url, wait_for_tts);
        });
        return;
    }
    StartRadioAfterStop(url, wait_for_tts);
}

void Application::StartRadioAfterStop(const std::string& url, bool wait_for_tts) {
    // 清掉上一次可能残留的等待状态
    pending_radio_wait_tts_ = false;
    if (radio_pending_timer_ != nullptr) {
        esp_timer_stop(radio_pending_timer_);
    }
    pending_radio_url_ = url;
    if (wait_for_tts) {
        // AI 工具调用：等本轮 tts stop（或兜底超时）再真正起流，避免 TTS 卡顿
        pending_radio_wait_tts_ = true;
        if (radio_pending_timer_ != nullptr) {
            esp_timer_start_once(radio_pending_timer_, kRadioWaitTtsTimeoutMs * 1000);
        }
    }
    MaybeStartPendingRadio();
}

void Application::MaybeStartPendingRadio() {
    if (pending_radio_url_.empty() || !radio_start_handler_) {
        return;
    }
    if (pending_radio_wait_tts_) {
        return;  // 等 tts stop 或兜底超时清标记
    }
    if (GetDeviceState() == kDeviceStateSpeaking) {
        return;  // 保守：仍在说话则不打断
    }
    auto url = std::move(pending_radio_url_);
    pending_radio_url_.clear();
    if (radio_pending_timer_ != nullptr) {
        esp_timer_stop(radio_pending_timer_);
    }
    radio_start_handler_(url);
    radio_switching_ = false;
    // 电台起流后自动进入空闲：不进入监听态，避免扬声器声音回灌麦克风
    auto state = GetDeviceState();
    if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::RadioPendingTimeoutCallback(void* arg) {
    auto* self = static_cast<Application*>(arg);
    self->Schedule([self]() {
        if (self->pending_radio_wait_tts_) {
            ESP_LOGW(TAG, "Radio TTS wait timeout (%d ms), starting anyway",
                     kRadioWaitTtsTimeoutMs);
            self->pending_radio_wait_tts_ = false;
            self->MaybeStartPendingRadio();
        }
    });
}

void Application::StopRadio(std::function<void()> on_stopped) {
    bool switching = on_stopped != nullptr;
    radio_switching_ = switching;
    pending_radio_url_.clear();
    pending_radio_wait_tts_ = false;
    radio_network_paused_ = false;
    radio_network_url_.clear();
    if (radio_pending_timer_ != nullptr) {
        esp_timer_stop(radio_pending_timer_);
    }
    radio_playing_ = false;
    if (!switching) {
        audio_service_.SetExternalPlaybackActive(false);
        SetBacklightForPlayback(false);
    }
    ArmRadioWatchdog(false);
    if (radio_stop_handler_) {
        // 电台 Stop() 可能阻塞等待 HTTP 段读取返回，放到一次性任务里执行，
        // 避免卡住主事件循环或 boot 按键任务。切台时完成后再起新流。
        auto* handler = new std::function<void()>([this, on_stopped]() {
            radio_stop_handler_();
            if (on_stopped) {
                Schedule([cb = on_stopped]() { cb(); });
            }
        });
        if (xTaskCreate([](void* arg) {
                auto* h = static_cast<std::function<void()>*>(arg);
                (*h)();
                delete h;
                vTaskDelete(nullptr);
            }, "radio_stop", 4096, handler, 2, nullptr) != pdPASS) {
            (*handler)();
            delete handler;
        }
    } else if (on_stopped) {
        on_stopped();
    }
}

void Application::ArmRadioWatchdog(bool arm) {
    if (radio_watchdog_timer_ == nullptr) {
        return;
    }
    if (arm && !radio_watchdog_armed_) {
        esp_timer_start_once(radio_watchdog_timer_, kRadioBufferingTimeoutMs * 1000);
        radio_watchdog_armed_ = true;
    } else if (!arm && radio_watchdog_armed_) {
        esp_timer_stop(radio_watchdog_timer_);
        radio_watchdog_armed_ = false;
    }
}

void Application::RadioWatchdogCallback(void* arg) {
    static_cast<Application*>(arg)->HandleRadioWatchdogTimeout();
}

void Application::HandleRadioWatchdogTimeout() {
    radio_watchdog_armed_ = false;
    ESP_LOGW(TAG, "Radio buffering timeout, restarting stream");
    Schedule([this]() {
        if (radio_source_ != nullptr && radio_playing_) {
            radio_source_->RestartCurrent();
        }
    });
}

void Application::NetworkResumeTimerCallback(void* arg) {
    static_cast<Application*>(arg)->HandleNetworkResumeTimeout();
}

void Application::HandleNetworkResumeTimeout() {
    // esp_timer 在 timer 任务上下文，投递回主任务处理播放状态
    Schedule([this]() {
        if (!network_connected_) {
            return;  // 去抖期间又断网了，放弃本次恢复
        }
        if (music_network_paused_) {
            music_network_paused_ = false;
            ESP_LOGI(TAG, "Network recovered, resuming music");
            if (!ResumeMusic()) {
                ESP_LOGW(TAG, "Failed to resume music after network recovery");
            }
        }
        if (radio_network_paused_) {
            radio_network_paused_ = false;
            std::string url = radio_network_url_;
            radio_network_url_.clear();
            if (!url.empty() && radio_playing_ && radio_source_ != nullptr) {
                ESP_LOGI(TAG, "Network recovered, reconnecting radio: %s", url.c_str());
                radio_source_->RestartCurrent();
            }
        }
    });
}

bool Application::TryResumeFromPlaybackError() {
    // 播放中断时先从 byte_pos 续播当前曲目；用户主动暂停交给原逻辑
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (!music_active_ || music_paused_) {
            return false;
        }
        if (music_resume_retries_ >= kMaxMusicResumeRetries) {
            ESP_LOGW(TAG, "Music resume retries exhausted (%d), giving up this track",
                     music_resume_retries_);
            return false;
        }
    }
    // 抓快照（此刻 GMF pipeline 尚未销毁，可读到 byte_pos/position/duration）
    if (!CapturePlaybackSnapshot()) {
        return false;
    }
    // FLAC 的 micro-flac 解码器需要流首的 fLaC 头；从中间 byte_pos Range 续播
    // 只会拿到裸音频帧，解码器报 INPUT_INVALID 失败。故 FLAC 续播改为从头重播。
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (playback_snapshot_ && music_cursor_ < music_order_.size()) {
            const auto& url = music_playlist_[music_order_[music_cursor_]].audio_url;
            const char* ext = strrchr(url.c_str(), '.');
            if (ext && strcasecmp(ext, ".flac") == 0) {
                playback_snapshot_->restart_from_start = true;
                ESP_LOGW(TAG, "FLAC track: resume from start (needs fLaC header)");
            }
        }
    }
    // 只停底层流，保留音乐会话与快照，不销毁 Notifying 状态
    notify_player_.Stop();
    audio_service_.ResetDecoder();
    music_resume_retries_++;
    int backoff = 1 << (music_resume_retries_ - 1);  // 1,2,4,8,16 s
    if (backoff > 16) backoff = 16;
    ESP_LOGW(TAG, "Playback interrupted, retry in %ds (attempt %d/%d)",
             backoff, music_resume_retries_, kMaxMusicResumeRetries);
    if (music_retry_timer_ != nullptr) {
        esp_timer_start_once(music_retry_timer_, backoff * 1000 * 1000);
    }
    return true;
}

void Application::MusicRetryTimerCallback(void* arg) {
    static_cast<Application*>(arg)->HandleMusicRetryTimeout();
}

void Application::HandleMusicRetryTimeout() {
    Schedule([this]() {
        // 停流期间若用户主动暂停/停止/切歌，或已恢复联网，交回相应流程
        {
            std::lock_guard<std::mutex> lock(music_mutex_);
            if (!music_active_ || music_paused_) {
                return;
            }
        }
        ESP_LOGI(TAG, "Retrying playback from snapshot (attempt %d)", music_resume_retries_);
        if (RestartFromSnapshot()) {
            // 续播成功：从新的播放 ID 起，重试计数在下次正常完成时清零
            ESP_LOGI(TAG, "Resumed playback from byte pos successfully");
        } else {
            ESP_LOGW(TAG, "Resume attempt failed, will retry if budget remains");
            // 启动失败不会走完成回调（没有新 playback），这里自行决定是否再试
            if (music_resume_retries_ < kMaxMusicResumeRetries) {
                music_resume_retries_++;
                int backoff = 1 << (music_resume_retries_ - 1);
                if (backoff > 16) backoff = 16;
                if (music_retry_timer_ != nullptr) {
                    esp_timer_start_once(music_retry_timer_, backoff * 1000 * 1000);
                }
            } else {
                // 重试彻底失败：清理会话停止播放
                ESP_LOGE(TAG, "Music resume failed after %d attempts, stopping",
                         music_resume_retries_);
                StopMusicPlayback();
            }
        }
    });
}

void Application::HandleRadioGiveUp() {
    // 断网挂起期间重连次数耗尽：保留电台会话，等网络恢复再重建，不真正停台
    if (radio_network_paused_) {
        ESP_LOGW(TAG, "Radio restarts exhausted while offline, keeping session for reconnect");
        return;
    }
    ESP_LOGW(TAG, "Radio restarts exhausted, stopping");
    StopRadio();
}

Application::MusicPlayMode Application::LoadPersistedMusicMode() {
    Settings settings("music", false);
    return settings.GetInt("play_mode", 0) == 1 ? MusicPlayMode::kShuffle
                                               : MusicPlayMode::kSequential;
}

int Application::LoadSelectedSource() {
    Settings settings("music", false);
    return settings.GetInt("selected_source", 0);
}

void Application::SaveSelectedSource(int index) {
    Settings settings("music", true);
    settings.SetInt("selected_source", index);
}

void Application::SetMusicMode(MusicPlayMode mode) {
    // 模式立即持久化，重启和后续“播放全部音乐”都从 NVS 读取
    {
        Settings settings("music", true);
        settings.SetInt("play_mode", mode == MusicPlayMode::kShuffle ? 1 : 0);
    }
    std::lock_guard<std::mutex> lock(music_mutex_);
    if (!music_active_ || music_playlist_.empty() || music_cursor_ >= music_order_.size()) {
        music_mode_ = mode;
        return;
    }
    size_t current = music_order_[music_cursor_];
    music_mode_ = mode;
    music_order_.resize(music_playlist_.size());
    for (size_t i = 0; i < music_order_.size(); ++i) {
        music_order_[i] = i;
    }
    if (mode == MusicPlayMode::kShuffle) {
        for (size_t i = music_order_.size(); i > 1; --i) {
            size_t j = esp_random() % i;
            std::swap(music_order_[i - 1], music_order_[j]);
        }
    }
    auto begin = std::find(music_order_.begin(), music_order_.end(), current);
    if (begin != music_order_.end()) {
        std::rotate(music_order_.begin(), begin, music_order_.end());
    }
    music_cursor_ = 0;
}

Application::MusicStatus Application::GetMusicStatus() const {
    MusicStatus status;
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        status.active = music_active_;
        status.paused = music_paused_;
        status.position_ms = music_position_ms_;
        status.duration_ms = static_cast<int>(notify_player_.GetGmfDurationMs());
        // 暂停后 Stop() 会清空 GMF 时长，用快照兜底，避免 WebUI 滑块 max 丢失
        if (status.duration_ms == 0 && music_paused_ && playback_snapshot_ != nullptr &&
            playback_snapshot_->valid) {
            status.duration_ms = static_cast<int>(playback_snapshot_->duration_ms);
        }
        status.single = music_single_;
        status.source_index = music_source_index_;
        status.total = static_cast<int>(music_playlist_.size());
        status.truncated = music_truncated_;
        status.mode = music_mode_;
        status.volume = Board::GetInstance().GetAudioCodec()->output_volume();
        if (music_active_ && music_cursor_ < music_order_.size()) {
            status.track_index = static_cast<int>(music_order_[music_cursor_]);
            status.title = music_playlist_[music_order_[music_cursor_]].title;
        }
    }
    return status;
}

void Application::HandleNotificationFinished(uint32_t playback_id, bool success) {
    // 忽略暂停时迟到的完成回调，否则会停掉会话并清掉暂停现场
    if (GetMusicStatus().paused) {
        return;
    }
    if (GetDeviceState() != kDeviceStateNotifying || notification_playback_id_ != playback_id) {
        return;
    }
    ESP_LOGI(TAG, "Notification playback %lu %s", static_cast<unsigned long>(playback_id),
             success ? "completed" : "failed");
    // 播放中断（解码/网络错误）不等于曲目结束：GMF 的完成回调在 ERROR/STOPPED 时
    // 也以 success=false 上报，且往往早于 WiFi 断网事件。不能据此判定为坏曲目而跳歌，
    // 否则断网时会连跳数首后停播。先尝试从 byte_pos 断点续播（有限次+退避）。
    if (!success && TryResumeFromPlaybackError()) {
        return;
    }
    StopNotification();

    MusicSource next;
    bool has_next = false;
    {
        std::lock_guard<std::mutex> lock(music_mutex_);
        if (!music_active_) {
            return;
        }
        if (music_single_) {
            ClearMusicSessionLocked();
            return;
        }
        music_fail_streak_ = success ? 0 : music_fail_streak_ + 1;
        if (success) {
            music_resume_retries_ = 0;  // 正常播完一首，清零续播重试计数
        }
        if (music_fail_streak_ < 3 && music_cursor_ + 1 < music_order_.size()) {
            ++music_cursor_;
            next = music_playlist_[music_order_[music_cursor_]];
            has_next = true;
        } else {
            if (music_fail_streak_ >= 3) {
                ESP_LOGW(TAG, "Music stopped after %d consecutive failures", music_fail_streak_);
            }
            ClearMusicSessionLocked();
        }
    }
    if (has_next) {
        StartNotification(next.audio_url, {}, next.username, next.password, next.title);
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    if (GetDeviceState() == kDeviceStateNotifying) {
        StopNotification();
    }

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        SetLowPowerIfIdle();                                  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        // May be called from outside the main task (e.g. board button
        // callbacks), so schedule the invocation instead of running it here
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateIdle) {
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateNotifying) {
        Schedule([this, wake_word]() {
            if (GetDeviceState() == kDeviceStateNotifying) {
                StopNotification();
                BeginWakeWordInvoke(wake_word);
            }
        });
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() { AbortSpeaking(kAbortReasonNone); });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // 音乐/电台还在播放时禁止进入低功耗待机
    if (IsRadioPlaying()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::SendTtsFinished() {
    Schedule([this]() {
        if (protocol_) {
            protocol_->SendTtsFinished();
        }
    });
}

bool Application::IsNotifyPlayerBusy() const {
    return notify_player_.IsBusy();
}

PlaybackScheduler& Application::GetPlaybackScheduler() {
    return *playback_scheduler_;
}

const PlaybackScheduler& Application::GetPlaybackScheduler() const {
    return *playback_scheduler_;
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
    Schedule([this]() {
        if (GetDeviceState() == kDeviceStateNotifying) {
            StopNotification();
        }
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
