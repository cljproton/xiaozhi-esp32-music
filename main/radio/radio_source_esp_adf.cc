#include "radio_source.h"
#include "application.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

extern "C" {
#include "esp_audio_simple_player.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_element.h"
#include "esp_hls_io.h"
#include "esp_gmf_io_http.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_fourcc.h"
#include "esp_timer.h"
#include "media_lib_adapter.h"
#include "media_lib_os.h"
#include "lwip/sockets.h"
}

#define TAG "RadioSourceHLS"

namespace radio {

// mono 16-bit @24 kHz, pre-rolled before the first I2S write so the stream
// starts with a full HLS ring buffer behind it.
// >1KB 的 vector 分配走 PSRAM (CONFIG_SPIRAM_USE_MALLOC), 但 insert 渐进增长
// 会让新旧缓冲同时驻留、翻倍峰值, 在分片加载时拿不到连续大块而 abort。
// 因此按 PSRAM 余量选档并一次性 reserve 到位, 把峰值压到 1 倍; PSRAM 紧张的
// 板子自动退回短预滚, 避免启动时拿不到连续大块。
constexpr size_t kPrebufferSamplesHi = 24000 * 6;   // 6 s
constexpr size_t kPrebufferSamplesLo = 24000 * 4;   // 4 s
// 自动重连时缩短预滚，避免每次断流都要多等才出声。
constexpr size_t kRestartPrebufferSamples = 24000 * 18 / 10;
// 6s 预滚需 ~288KB 连续块 + HLS ring 2MB; 余量低于此值退回 4s。
constexpr size_t kPrebufferHiPsramThreshold = 3 * 1024 * 1024;

// 按启动时 PSRAM 余量选预滚档位：大块连续分配留足余量，避免 realloc 失败。
size_t SelectPrebufferSamples() {
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return psram_free >= kPrebufferHiPsramThreshold ? kPrebufferSamplesHi : kPrebufferSamplesLo;
}

// 连续失败上限：避免无效 URL 无限重连、麦克风一直静音。
constexpr int kMaxConsecutiveRestarts = 10;
// 借鉴 RadioDroid 的 settings_retry_delay=100ms: 固定短延迟快速重试。
// 之前用 backoff_s*1000 传给微秒级 esp_timer, 实为 1~16ms, 网络抖动时
// 1 秒内连击耗尽 10 次机会; 100ms 节奏给网络恢复留出窗口。
constexpr uint64_t kRetryDelayUs = 100 * 1000;

// 海外台链路抖动时 10s 读超时会被误判为断流并触发整机重建（实测静音 18s），
// 保持 vendor 默认的 30s 读超时，配合 1MB 环形缓冲吸收抖动。
constexpr int kHttpReadTimeoutMs = 30 * 1000;

class RadioSourceHLS : public RadioSource {
public:
    RadioSourceHLS()
        : player_(nullptr), hls_io_(nullptr), state_(RadioState::kStopped), volume_(100) {
        media_lib_add_default_adapter();
        station_manager_.Init();
        esp_timer_create_args_t restart_backoff_timer_args = {
            .callback = &RadioSourceHLS::RestartBackoffTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "radio_restart_backoff",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&restart_backoff_timer_args, &restart_backoff_timer_));
    }

    ~RadioSourceHLS() {
        shutting_down_ = true;
        Stop();
        // 等后台拆除任务结束（含收尾重启）再释放成员
        while (teardown_active_) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (restart_backoff_timer_ != nullptr) {
            esp_timer_stop(restart_backoff_timer_);
            esp_timer_delete(restart_backoff_timer_);
            restart_backoff_timer_ = nullptr;
        }
        if (buffer_log_timer_ != nullptr) {
            esp_timer_stop(buffer_log_timer_);
            esp_timer_delete(buffer_log_timer_);
            buffer_log_timer_ = nullptr;
        }
    }

    bool Start(const std::string& url) override { return StartInternal(url, false); }
    bool StartInternal(const std::string& url, bool quick_preroll) {
        ESP_LOGI(TAG, "Start: %s", url.c_str());
        // Stop() can arrive from the button task while the MCP task starts a
        // station; the player handle is not safe to touch concurrently.
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);

        if (state_ == RadioState::kPlaying || state_ == RadioState::kBuffering) {
            ESP_LOGW(TAG, "Already playing");
            return false;
        }
        // New session: bump epoch so stale events from a previous stop/restart
        // (e.g. a STOPPED delivered during a silent restart) are dropped.
        epoch_++;
        // 用户显式开台时取消拆除任务里未决的自动重启
        {
            std::lock_guard<std::mutex> lock(url_mutex_);
            restart_url_.clear();
        }

        current_url_ = url;
        // Look up station gain from station manager (persisted in NVS).
        auto station_opt = station_manager_.FindByUrl(url);
        if (station_opt) {
            gain_ = std::clamp(station_opt->gain, 50, 300);
            ESP_LOGI(TAG, "Using station gain: %d%% for %s", gain_, station_opt->name.c_str());
        } else {
            // Fallback: legacy hardcoded gain for ifeng
            gain_ = url.find("playtv-live.ifeng.com") != std::string::npos ? 200 : 100;
            ESP_LOGI(TAG, "No station entry, using fallback gain: %d%%", gain_);
        }
        state_ = RadioState::kConnecting;
        // 按 PSRAM 余量选档: 大块连续分配留足余量, 紧张板子退回 4s 防 abort。
        prebuffer_target_ = quick_preroll ? kRestartPrebufferSamples : SelectPrebufferSamples();
        preroll_.clear();
        // 一次性预留完整预滚容量: 避免 insert 渐进增长时新旧缓冲翻倍峰值,
        // 在 HLS 分片加载的 PSRAM 紧张窗口拿不到连续大块而 abort。
        preroll_.reserve(prebuffer_target_);
        preroll_done_ = false;
        quick_preroll_ = quick_preroll;  // 用户开台走完整预滚，自动重连走短预滚
        mute_ = false;
        {
            std::lock_guard<std::mutex> lock(url_mutex_);
            last_url_ = url;
        }
        // The first PRE_REQUEST hook fire is this player's initial connection;
        // any later one means the HTTP IO silently reconnected (server reset).
        expect_first_request_ = true;
        restart_pending_ = false;
        stop_requested_ = false;

        esp_asp_cfg_t cfg = {};
        cfg.out.cb = [](uint8_t* data, int data_size, void* ctx) -> int {
            auto* self = static_cast<RadioSourceHLS*>(ctx);
            if (data_size <= 0 || !self->pcm_callback_) {
                return data_size;
            }
            int16_t* samples = reinterpret_cast<int16_t*>(data);
            size_t count = data_size / sizeof(int16_t);
            int gain = self->mute_ ? 0 : self->volume_ * self->gain_ / 100;
            if (gain != 100) {
                for (size_t i = 0; i < count; i++) {
                    int32_t v = static_cast<int32_t>(samples[i]) * gain / 100;
                    if (v > 32767) {
                        v = 32767;
                    } else if (v < -32768) {
                        v = -32768;
                    }
                    samples[i] = static_cast<int16_t>(v);
                }
            }
            if (!self->preroll_done_) {
                self->preroll_.insert(self->preroll_.end(), samples, samples + count);
                if (self->preroll_.size() < self->prebuffer_target_) {
                    // Not touching I2S lets the decoder drain the HLS ring
                    // buffer into the preroll at full speed.
                    return data_size;
                }
                std::vector<int16_t> pcm = std::move(self->preroll_);
                self->preroll_done_ = true;
                ESP_LOGI(TAG, "Preroll complete: %zu samples buffered", pcm.size());
                self->pcm_callback_(std::move(pcm), self->sample_rate_, self->channels_);
                return data_size;
            }
            // Detect potential underrun: if callback is called with very small count
            if (count < 256) {
                ESP_LOGW(TAG, "Small PCM batch: %zu samples (possible underrun)", count);
            }
            std::vector<int16_t> pcm(samples, samples + count);
            self->pcm_callback_(std::move(pcm), self->sample_rate_, self->channels_);
            return data_size;
        };
        cfg.out.user_ctx = this;
        cfg.task_prio = 6;
        cfg.task_stack = 6 * 1024;
        // 解码任务固定 core 1，与 audio output 同核；core 0 留给 HTTP IO(TLS 握手)与 WiFi，
        // 避免 TLS 解密抢占解码/I2S 导致卡顿。
        cfg.task_core = 1;

        if (esp_audio_simple_player_new(&cfg, &player_) != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "player_new failed");
            state_ = RadioState::kError;
            return false;
        }

        esp_gmf_pool_handle_t pool = nullptr;
        esp_audio_simple_player_get_pool(player_, &pool);

        // The player's default HTTP IO carries no CA bundle, so HTTPS HLS
        // fails TLS setup. Register an equivalent IO wired to the ESP-IDF
        // certificate bundle so https:// playlists work too.
        {
            http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
            http_cfg.dir = ESP_GMF_IO_DIR_READER;
            http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
            http_cfg.user_data = this;
            // 默认环只有 20KB，64kbps 直连流仅能扛 2.5s 抖动；扩到 1MB
            // （HLS 的 playlist/分片请求也走此 IO，另有独立的 2MB HLS 环）。
            http_cfg.io_cfg.buffer_cfg.buffer_size = 1 * 1024 * 1024;
            // 网络侧任务固定 core 0（与 WiFi 协议栈同核），解码/输出在 core 1。
            http_cfg.io_cfg.thread.core = 0;
            // 每次建立连接（含服务器断开后的静默重连）都会回调 PRE_REQUEST。
            // 直连流重连后靠这个事件触发解码器重置（见 RestartInPlace），不拆管线。
            http_cfg.event_handle = [](http_stream_event_msg_t* msg) -> int {
                auto* self = static_cast<RadioSourceHLS*>(msg->user_data);
                // vendor 的 esp_gmf_io_http 把读超时硬编码为 30s 且无配置入口，
                // 这里用官方 API 在每个客户端句柄上收窄，缩短 Stop 等待与断流检测。
                if (msg->http_client) {
                    esp_http_client_handle_t client =
                        static_cast<esp_http_client_handle_t>(msg->http_client);
                    esp_http_client_set_timeout_ms(client, kHttpReadTimeoutMs);
                    int fd = esp_http_client_get_socket(client);
                    if (fd >= 0) {
                        // lwIP TCP keepalive（空闲 20s / 探测间隔 10s / 3 次）:
                        // NAT 映射与网络抖动时及时探测，避免长时间假死。
                        int one = 1;
                        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
                        int idle = 20, intvl = 10, cnt = 3;
                        setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
                        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
                    }
                }
                if (msg->event_id == HTTP_STREAM_PRE_REQUEST) {
                    std::string url;
                    {
                        std::lock_guard<std::mutex> lock(self->url_mutex_);
                        url = self->last_url_;
                    }
                    // HLS 每个分片都会新建 HTTP 请求，这是正常行为，不是断线。
                    // 只有直连流才把第二次 PRE_REQUEST 当作服务器重置。
                    if (url.find(".m3u8") != std::string::npos) {
                        return ESP_GMF_ERR_OK;
                    }
                    // 直连流断流：GMF HTTP IO 在同一 IO 句柄上自行重连；此时只重置解码器
                    // 元素，不拆 player/pipeline（对齐 RadioDroid IcyDataSource 层重试），
                    // 解码器在新连接数据到达后重新打开，避免重建管线导致解码上下文丢失。
                    if (!self->expect_first_request_.exchange(false) &&
                        !self->restart_pending_.exchange(true)) {
                        self->http_start_time_ = esp_timer_get_time() / 1000;
                        ESP_LOGW(TAG, "HTTP reconnect detected, resetting audio decoder only");
                        auto& app = Application::GetInstance();
                        app.Schedule([self]() {
                            std::string url;
                            {
                                std::lock_guard<std::mutex> lock(self->url_mutex_);
                                url = self->last_url_;
                            }
                            if (!url.empty() && self->state_ == RadioState::kPlaying) {
                                self->RestartInPlace(url);
                            }
                            self->restart_pending_ = false;
                        });
                    }
                }
                if (msg->event_id == HTTP_STREAM_ON_RESPONSE) {
                    // 每 3KB 一次，I 级在 115200 串口下每片要阻塞约 100ms
                    ESP_LOGV(TAG, "HTTP data: %d bytes", msg->buffer_len);
                }
                if (msg->event_id == HTTP_STREAM_FINISH_REQUEST) {
                    ESP_LOGI(TAG, "HTTP request finished, took %lld ms",
                             (long long)(esp_timer_get_time() / 1000 - self->http_start_time_));
                }
                return ESP_GMF_ERR_OK;
            };
            esp_gmf_io_handle_t https_io = nullptr;
            if (esp_gmf_io_http_init(&http_cfg, &https_io) == ESP_GMF_ERR_OK) {
                esp_gmf_pool_register_io(pool, https_io, nullptr);
            } else {
                ESP_LOGW(TAG, "HTTPS IO init failed, https stations unavailable");
            }
        }

        esp_hls_io_cfg_t hls_cfg = {};
        hls_cfg.pool = pool;
        hls_cfg.name = "io_hls";
        // 2MB PSRAM ring buffer absorbs network jitter instead of underrunning.
        // hls_io only overrides fields > 0, so the rest of its defaults hold.
        // NOTE: keep at 2MB — a 4MB bump exhausted PSRAM and failed the GMF
        // data bus allocation on the 8MB-PSRAM cube boards.
        hls_cfg.io_cfg.buffer_cfg.buffer_size = 2 * 1024 * 1024;
        hls_cfg.io_cfg.thread.core = 0;  // 网络侧（playlist 解析）与 HTTP IO 同核
        hls_cfg.file_seg_cb = [](esp_hls_file_seg_info_t* info, void* ctx) -> int {
            if (info) {
                ESP_LOGI(TAG, "Segment format: %s", ESP_FOURCC_TO_STR(info->format));
            }
            return 0;
        };
        hls_cfg.ctx = this;

        if (esp_gmf_io_hls_init(&hls_cfg, &hls_io_) != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "hls io init failed");
            esp_audio_simple_player_destroy(player_);
            player_ = nullptr;
            state_ = RadioState::kError;
            return false;
        }
        esp_audio_simple_player_register_io(player_, hls_io_);

        ESP_LOGI(TAG, "Buffer %u KB, preroll %u ms, internal free %u KB, PSRAM free %u KB",
                 (unsigned)(hls_cfg.io_cfg.buffer_cfg.buffer_size / 1024),
                 (unsigned)(prebuffer_target_ * 1000 / 24000),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

        esp_audio_simple_player_set_event(player_, [](esp_asp_event_pkt_t* pkt, void* ctx) -> int {
            auto* self = static_cast<RadioSourceHLS*>(ctx);
            if (!pkt) {
                return 0;
            }
            if (pkt->type == ESP_ASP_EVENT_TYPE_STATE &&
                pkt->payload_size == sizeof(esp_asp_state_t)) {
                esp_asp_state_t st = *(esp_asp_state_t*)pkt->payload;
                ESP_LOGI(TAG, "Player state: %s", esp_audio_simple_player_state_to_str(st));
                // 异步投递到主任务，避免在 GMF 管道任务中直接调用状态回调导致死锁
                auto& app = Application::GetInstance();
                uint32_t epoch = self->epoch_.load();
                app.Schedule([self, st, epoch]() {
                    // Drop stale transitions from a previous stop/restart session.
                    if (epoch != self->epoch_.load()) {
                        return;
                    }
                    switch (st) {
                        case ESP_ASP_STATE_RUNNING: {
                            self->SetState(RadioState::kPlaying, "Playing");
                            self->restart_count_ = 0;
                            // Session (re)started: Application mutes the mic via SetRadioPlaying.
                            Application::GetInstance().SetRadioPlaying(true);
                            break;
                        }
                        case ESP_ASP_STATE_PAUSED:
                            self->SetState(RadioState::kStopped, "Paused");
                            break;
                        case ESP_ASP_STATE_STOPPED:
                        case ESP_ASP_STATE_FINISHED:
                            self->SetState(RadioState::kStopped, "Stopped");
                            // 意外停止（非用户发起）：与 kError 一样自动重启，最多 5 次。
                            // 防止服务器 reset 竞争后停在 stopped 静默死亡。
                            if (!self->stop_requested_) {
                                std::string url;
                                {
                                    std::lock_guard<std::mutex> lock(self->url_mutex_);
                                    url = self->last_url_;
                                }
                                if (!url.empty()) {
                                    self->TriggerAutoRestart(url, RadioState::kStopped);
                                }
                            }
                            break;
                        case ESP_ASP_STATE_ERROR:
                            self->SetState(RadioState::kError, "Error");
                            // 整条管道报错（如重连次数耗尽）时重建管道
                            {
                                std::string url;
                                {
                                    std::lock_guard<std::mutex> lock(self->url_mutex_);
                                    url = self->last_url_;
                                }
                                if (!url.empty()) {
                                    self->TriggerAutoRestart(url, RadioState::kError);
                                }
                            }
                            break;
                        default:
                            break;
                    }
                });
                return 0;
            }
            if (pkt->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO &&
                pkt->payload_size == sizeof(esp_asp_music_info_t)) {
                const esp_asp_music_info_t* info = (const esp_asp_music_info_t*)pkt->payload;
                ESP_LOGI(TAG, "Music info: rate=%d ch=%d bitrate=%d bits=%d",
                         info->sample_rate, info->channels, info->bitrate, info->bits);
                // 如果解码器上报的采样率与预期不一致，记录警告
                if (info->sample_rate != 0 && info->sample_rate != self->sample_rate_) {
                    ESP_LOGW(TAG, "Sample rate changed: expected %d, got %d",
                             self->sample_rate_, info->sample_rate);
                }
                // 更新内部记录，供回调使用
                self->sample_rate_ = info->sample_rate;
                self->channels_ = info->channels;
                return 0;
            }
            return 0;
        }, this);

        if (esp_audio_simple_player_run(player_, current_url_.c_str(), nullptr) != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "player_run failed for URL: %s", current_url_.c_str());
            
            // If HTTPS failed, try HTTP fallback once
            if (current_url_.rfind("https://", 0) == 0) {
                std::string http_url = "http://" + current_url_.substr(8); // replace https:// with http://
                ESP_LOGI(TAG, "HTTPS failed, trying HTTP fallback: %s", http_url.c_str());
                current_url_ = http_url;
                if (esp_audio_simple_player_run(player_, current_url_.c_str(), nullptr) == ESP_GMF_ERR_OK) {
                    ESP_LOGI(TAG, "HTTP fallback succeeded");
                    state_ = RadioState::kBuffering;
                    station_manager_.RecordPlay(current_url_);
                    return true;
                }
                ESP_LOGW(TAG, "HTTP fallback also failed");
            }
            
            ESP_LOGE(TAG, "player_run failed");
            esp_audio_simple_player_destroy(player_);
            player_ = nullptr;
            hls_io_ = nullptr;
            state_ = RadioState::kError;
            return false;
        }

        state_ = RadioState::kBuffering;
        // Keep "last played" current so next/prev step from this station.
        station_manager_.RecordPlay(current_url_);

        // Start periodic buffer water level logging
        if (buffer_log_timer_ == nullptr) {
            esp_timer_create_args_t timer_args = {
                .callback = [](void* arg) {
                    auto* self = static_cast<RadioSourceHLS*>(arg);
                    if (self->state_ == RadioState::kPlaying || self->state_ == RadioState::kBuffering) {
                        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
                        size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
                        ESP_LOGI(TAG, "Buffer stats: internal=%zu KB, PSRAM=%zu KB, preroll_done=%d, preroll_size=%zu",
                                 internal_free, psram_free, self->preroll_done_, self->preroll_.size());
                    }
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "radio_buffer_log",
                .skip_unhandled_events = true,
            };
            ESP_ERROR_CHECK(esp_timer_create(&timer_args, &buffer_log_timer_));
            ESP_ERROR_CHECK(esp_timer_start_periodic(buffer_log_timer_, 5 * 1000 * 1000)); // 5s
        }

        return true;
    }

    void Stop() override {
        // 快速路径：拆除进行中只更新意图（清掉未决重启），不等拆除完成。
        // lifecycle_mutex_ 可能被拆除任务持有长达 30s+，不能让 MCP/web/主任务卡住。
        if (teardown_active_.load()) {
            stop_requested_ = true;
            mute_ = true;
            restart_pending_ = false;
            expect_first_request_ = false;
            state_ = RadioState::kStopped;
            {
                std::lock_guard<std::mutex> lock(url_mutex_);
                restart_url_.clear();
                last_url_.clear();
            }
            current_url_.clear();
            if (buffer_log_timer_ != nullptr) {
                esp_timer_stop(buffer_log_timer_);
                esp_timer_delete(buffer_log_timer_);
                buffer_log_timer_ = nullptr;
            }
            return;
        }
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!player_ && state_ == RadioState::kStopped) {
            return;
        }
        ESP_LOGI(TAG, "Stop");
        // 标记用户/主动停止，STOPPED 事件不触发自动重启，并取消未决的自动重启
        stop_requested_ = true;
        {
            std::lock_guard<std::mutex> lock2(url_mutex_);
            restart_url_.clear();
        }
        // 先把输出静音，让用户立刻听到"停止"，后台任务再慢慢拆除
        mute_ = true;
        if (player_) {
            teardown_active_ = true;
            if (xTaskCreate(TeardownEntry, "radio_teardown", 4096, this, 5, nullptr) != pdPASS) {
                // 任务创建失败则退回同步拆除，保证 player 最终被释放
                teardown_active_ = false;
                esp_audio_simple_player_stop(player_);
                esp_audio_simple_player_destroy(player_);
                player_ = nullptr;
                hls_io_ = nullptr;
            }
        }
        state_ = RadioState::kStopped;
        current_url_.clear();
        {
            std::lock_guard<std::mutex> lock(url_mutex_);
            last_url_.clear();
        }
        expect_first_request_ = false;
        restart_pending_ = false;
        if (buffer_log_timer_ != nullptr) {
            esp_timer_stop(buffer_log_timer_);
            esp_timer_delete(buffer_log_timer_);
            buffer_log_timer_ = nullptr;
        }
    }

    void RestartCurrent() override {
        std::string url;
        {
            std::lock_guard<std::mutex> lock(url_mutex_);
            url = last_url_;
        }
        if (url.empty()) {
            ESP_LOGW(TAG, "RestartCurrent: no URL to restart");
            return;
        }
        ESP_LOGW(TAG, "RestartCurrent: %s", url.c_str());
        Restart(url);
    }

    void Pause() override {
        if (player_) {
            esp_audio_simple_player_pause(player_);
        }
    }

    void Resume() override {
        if (player_) {
            esp_audio_simple_player_resume(player_);
        }
    }

    void SetVolume(int volume) override {
        volume_ = std::clamp(volume, 0, 200);
    }

    int GetVolume() const override { return volume_.load(); }
    RadioState GetState() const override { return state_.load(); }
    bool IsPlaying() const override { return state_ == RadioState::kPlaying; }
    std::string GetCurrentUrl() const override { return current_url_; }

    void SetPcmCallback(PcmCallback cb) override { pcm_callback_ = cb; }
    void SetMetadataCallback(MetadataCallback cb) override { metadata_callback_ = cb; }
    void SetStateCallback(StateCallback cb) override { state_callback_ = cb; }

    bool SetStation(const StationInfo& station) override { return Start(station.url); }

    StationInfo GetCurrentStation() const override {
        StationInfo info;
        info.url = current_url_;
        return info;
    }

    StationManager& GetStationManager() override { return station_manager_; }

    void SetPlaybackMode(PlaybackMode mode) override {}

private:
    // 自动重连：固定 100ms 短延迟（对齐 RadioDroid 重试节奏）后由主任务回调
    // 统一执行重启，避免在 esp_timer task 上下文操作播放器。要求调用方在主任务
    // 上下文（如 Application::Schedule 投递的任务）内。
    void TriggerAutoRestart(const std::string& url, RadioState require_state) {
        if (url.empty() || stop_requested_ || state_ != require_state) {
            restart_pending_ = false;
            return;
        }
        // 连续失败太多次时停止，避免无效 URL 导致无限重连、麦克风长期静音
        if (restart_count_ >= kMaxConsecutiveRestarts) {
            restart_pending_ = false;
            ESP_LOGE(TAG, "Auto-restart gave up after %d consecutive failures", restart_count_);
            auto& app = Application::GetInstance();
            // 断网期间只挂起会话、保留电台，等网络恢复重连；联网时才真正停台
            app.Schedule([]() { Application::GetInstance().HandleRadioGiveUp(); });
            return;
        }
        // 去重：HTTP 重连回调若已排队自己的重启（已置位）就跳过；干净 EOF
        // 没有 HTTP 回调置位，必须继续往下排，否则播完 30s 就静默停住。
        if (restart_pending_.exchange(true)) {
            return;
        }
        restart_count_++;
        ESP_LOGW(TAG, "Auto-restarting stream (attempt %d)", restart_count_);
        esp_timer_start_once(restart_backoff_timer_, kRetryDelayUs);
    }

    static void RestartBackoffTimerCallback(void* arg) {
        auto* self = static_cast<RadioSourceHLS*>(arg);
        // 退避期结束：回到主任务上下文评估是否自动重启
        auto& app = Application::GetInstance();
        app.Schedule([self]() {
            if (self->restart_pending_ && !self->stop_requested_) {
                std::string url;
                {
                    std::lock_guard<std::mutex> lock(self->url_mutex_);
                    url = self->last_url_;
                }
                if (!url.empty()) {
                    self->Restart(url);
                }
                self->restart_pending_ = false;
            }
        });
    }

    // 后台拆除任务：HLS fetcher（预编译库）拆除时会继续重试拉流，可拖 30s+，
    // 不能阻塞调用方（MCP/web/按钮）与主事件循环。拆完后若 restart_url_ 非空
    // 且未被用户叫停，自动重启该 URL（断线恢复路径）。
    static void TeardownEntry(void* arg) {
        auto* self = static_cast<RadioSourceHLS*>(arg);
        std::string restart;
        {
            std::lock_guard<std::mutex> lock(self->lifecycle_mutex_);
            if (self->player_) {
                esp_audio_simple_player_stop(self->player_);
                // HLS IO 由 player 内部的 pool 管理，销毁 player 时会自动清理
                esp_audio_simple_player_destroy(self->player_);
                self->player_ = nullptr;
                self->hls_io_ = nullptr;
            }
        }
        {
            std::lock_guard<std::mutex> lock(self->url_mutex_);
            restart = self->restart_url_;
            self->restart_url_.clear();
        }
        self->teardown_active_ = false;
        // Stop periodic timers before potential restart
        if (self->buffer_log_timer_ != nullptr) {
            esp_timer_stop(self->buffer_log_timer_);
            esp_timer_delete(self->buffer_log_timer_);
            self->buffer_log_timer_ = nullptr;
        }
        if (!restart.empty() && !self->stop_requested_ && !self->shutting_down_ &&
            self->state_ != RadioState::kPlaying) {
            ESP_LOGI(TAG, "Teardown done, restarting %s", restart.c_str());
            self->StartInternal(restart, true);
        }
        // 最后清除：析构函数据此等待全部收尾（含可能的自动重启）完成
        self->teardown_active_ = false;
        vTaskDelete(nullptr);
    }

    // 断流恢复（IO 层已自动重连成功）：只重置解码器元素，不拆 pipeline/player。
    // 解码器通过 need_reopen 在下一次 process 时以新连接数据重新打开，IO 环、
    // rate/ch cvt、输出回调等上游全部保留，AAC 同步状态不会丢失。
    void RestartInPlace(const std::string& url) {
        if (stop_requested_ || teardown_active_.load() || !player_) {
            return;
        }
        esp_gmf_pipeline_handle_t pipe = nullptr;
        if (esp_audio_simple_player_get_pipeline(player_, &pipe) != ESP_GMF_ERR_OK || pipe == nullptr) {
            ESP_LOGW(TAG, "RestartInPlace: pipeline unavailable");
            Restart(url);
            return;
        }
        esp_gmf_element_handle_t head = nullptr;
        if (esp_gmf_pipeline_get_head_el(pipe, &head) != ESP_GMF_ERR_OK || head == nullptr) {
            ESP_LOGW(TAG, "RestartInPlace: head element unavailable");
            Restart(url);
            return;
        }
        // 用解码器当前配置原样重配置：只置 need_reopen，下次 process 时以新连接数据
        // 重新打开解码器。不按 URL 猜格式，编码类型与子配置都保持原值，不会变声。
        esp_audio_simple_dec_cfg_t* cur_cfg =
            static_cast<esp_audio_simple_dec_cfg_t*>(OBJ_GET_CFG(head));
        if (cur_cfg == nullptr || cur_cfg->dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
            ESP_LOGW(TAG, "RestartInPlace: decoder config unavailable");
            Restart(url);
            return;
        }
        if (esp_gmf_audio_dec_reconfig(head, cur_cfg) == ESP_GMF_ERR_OK) {
            ESP_LOGI(TAG, "RestartInPlace: decoder reset scheduled (type=%d)", (int)cur_cfg->dec_type);
            return;
        }
        ESP_LOGW(TAG, "RestartInPlace: decoder reset failed");
        Restart(url);
    }

    // 断线恢复专用：静音并异步拆除当前管道，拆完后由 TeardownEntry 重启 url。
    // 任何分支都不允许长时间持有 lifecycle_mutex_（主任务也会走到这里）。
    void Restart(const std::string& url) {
        stop_requested_ = false;      // 非用户停止
        expect_first_request_ = true;
        restart_pending_ = false;
        if (teardown_active_.load()) {
            // 拆除进行中：只更新重启意图（TeardownEntry 收尾时读取），不等待
            std::lock_guard<std::mutex> lock(url_mutex_);
            restart_url_ = url;
            if (buffer_log_timer_ != nullptr) {
                esp_timer_stop(buffer_log_timer_);
                esp_timer_delete(buffer_log_timer_);
                buffer_log_timer_ = nullptr;
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            if (teardown_active_.load()) {
                std::lock_guard<std::mutex> lock2(url_mutex_);
                restart_url_ = url;
                return;
            }
            state_ = RadioState::kStopped;  // Start() 要求非 playing/buffering
            mute_ = true;
            if (player_) {
                {
                    std::lock_guard<std::mutex> lock2(url_mutex_);
                    restart_url_ = url;
                }
                teardown_active_ = true;
                if (xTaskCreate(TeardownEntry, "radio_teardown", 4096, this, 5, nullptr) != pdPASS) {
                    teardown_active_ = false;
                    {
                        std::lock_guard<std::mutex> lock2(url_mutex_);
                        restart_url_.clear();
                    }
                    state_ = RadioState::kError;
                }
                return;
            }
        }
        Start(url);  // 无 player 可拆，直接重启（Start 自行加锁）
        if (buffer_log_timer_ != nullptr) {
            esp_timer_stop(buffer_log_timer_);
            esp_timer_delete(buffer_log_timer_);
            buffer_log_timer_ = nullptr;
        }
    }

    void SetState(RadioState st, const std::string& message) {
        state_ = st;
        if (state_callback_) {
            state_callback_(st, message);
        }
    }

    esp_asp_handle_t player_ = nullptr;
    std::atomic<uint32_t> epoch_{0};
    esp_gmf_io_handle_t hls_io_ = nullptr;
    std::atomic<RadioState> state_;
    std::atomic<int> volume_;
    std::string current_url_;
    std::string last_url_;
    std::mutex url_mutex_;
    std::atomic<bool> expect_first_request_{false};
    std::atomic<bool> restart_pending_{false};
    esp_timer_handle_t restart_backoff_timer_ = nullptr;
    esp_timer_handle_t buffer_log_timer_ = nullptr;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> mute_{false};
    std::atomic<bool> teardown_active_{false};
    std::atomic<bool> shutting_down_{false};
    // lifecycle_mutex_ 保护；非空表示拆除结束后要自动重启的 URL
    std::string restart_url_;
    int64_t http_start_time_ = 0;
    int sample_rate_ = 24000;
    int channels_ = 1;
    int gain_ = 100;
    std::mutex lifecycle_mutex_;
    std::vector<int16_t> preroll_;
    size_t prebuffer_target_ = kPrebufferSamplesHi;
    bool preroll_done_ = false;
    std::atomic<bool> quick_preroll_{false};
    PcmCallback pcm_callback_;
    MetadataCallback metadata_callback_;
    StateCallback state_callback_;
    StationManager station_manager_;
    int restart_count_ = 0;
};

std::shared_ptr<RadioSource> CreateRadioSource() {
    return std::make_shared<RadioSourceHLS>();
}

}  // namespace radio
