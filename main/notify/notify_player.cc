#include "notify_player.h"

#include <algorithm>
#include <string>
#include <vector>

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include <string.h>
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_audio_element.h"
#include "esp_gmf_audio_helper.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_err.h"
#include "esp_gmf_event.h"
#include "esp_gmf_io.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_task.h"
#include "esp_http_client.h"

#include "board.h"

namespace {
constexpr uint32_t kNotifyTaskStackSize = 6144;
constexpr UBaseType_t kNotifyTaskPriority = 2;
const char* TAG = "NotifyPlayer";
static int s_audio_dec_reg_ref_count = 0;

bool IsSupportedUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

}  // namespace

NotifyPlayer::NotifyPlayer(AudioService& audio_service) : audio_service_(audio_service) {}

NotifyPlayer::~NotifyPlayer() { Stop(); }

bool NotifyPlayer::Start(std::string audio_url, std::vector<NotifySubtitle> subtitles,
                         uint32_t playback_id, SubtitleCallback subtitle_callback,
                         FinishedCallback finished_callback, const std::string& username,
                         const std::string& password, uint64_t start_byte_pos, uint32_t start_pos_ms,
                         uint32_t start_duration_ms) {
    if (playback_id == 0 || !IsSupportedUrl(audio_url)) {
        return false;
    }

    std::stable_sort(subtitles.begin(), subtitles.end(),
                     [](const NotifySubtitle& left, const NotifySubtitle& right) {
                         return left.start_ms < right.start_ms;
                     });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ || worker_running_) {
            return false;
        }
        audio_url_ = std::move(audio_url);
        username_ = username;
        password_ = password;
        subtitles_ = std::move(subtitles);
        displayed_text_.clear();
        subtitle_callback_ = std::move(subtitle_callback);
        finished_callback_ = std::move(finished_callback);
        playback_id_ = playback_id;
        start_byte_pos_ = start_byte_pos;
        start_pos_ms_ = start_pos_ms;
        start_duration_ms_ = start_duration_ms;
        underrun_count_ = 0;
        next_subtitle_index_ = 0;
        active_ = true;
        worker_running_ = true;
        cancelled_ = false;
        http_finished_ = false;
        stream_started_ = false;
        playback_drained_ = false;
        completion_reported_ = false;
        gmf_state_ = ESP_GMF_EVENT_STATE_NONE;
        gmf_sample_rate_ = 0;
        gmf_channels_ = 1;
        gmf_bits_ = 16;
        gmf_playback_pos_ms_ = 0;
        gmf_duration_ms_ = 0;
    }

    BaseType_t created = xTaskCreate(WorkerEntry, "notify_http", kNotifyTaskStackSize, this,
                                     kNotifyTaskPriority, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create notification HTTP task");
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        worker_running_ = false;
        cancelled_ = true;
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void NotifyPlayer::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        active_ = false;
        subtitles_.clear();
        subtitle_callback_ = nullptr;
        finished_callback_ = nullptr;
    }
    StopGmfPlayback();
    // 等旧的工作线程收尾，避免紧接着的 Start() 因 worker_running_ 被拒
    for (int i = 0; i < 200; ++i) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!worker_running_) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

bool NotifyPlayer::IsActive(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && (playback_id == 0 || playback_id == playback_id_);
}

bool NotifyPlayer::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ || worker_running_;
}

bool NotifyPlayer::IsCancelled(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_ || !active_ || playback_id != playback_id_;
}

void NotifyPlayer::OnPlaybackProgress(uint32_t playback_id, uint32_t media_position_ms) {
    SubtitleCallback callback;
    std::string text;
    bool subtitle_changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || playback_id != playback_id_) {
            return;
        }
        gmf_playback_pos_ms_ = media_position_ms;
        while (next_subtitle_index_ < subtitles_.size() &&
               subtitles_[next_subtitle_index_].start_ms <= media_position_ms) {
            text = subtitles_[next_subtitle_index_].text;
            ++next_subtitle_index_;
            subtitle_changed = true;
        }
        if (!subtitle_changed || text == displayed_text_) {
            return;
        }
        displayed_text_ = text;
        callback = subtitle_callback_;
    }
    if (callback) {
        callback(playback_id, text);
    }
}

NotifyPlayer::FinishedCallback NotifyPlayer::CompleteLocked(uint32_t& playback_id) {
    if (completion_reported_ || !active_) {
        return nullptr;
    }
    completion_reported_ = true;
    active_ = false;
    playback_id = playback_id_;
    return finished_callback_;
}

void NotifyPlayer::OnPlaybackDrained() {
    FinishedCallback callback;
    uint32_t playback_id = 0;
    uint32_t underrun_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return;
        }
        playback_drained_ = true;
        if (http_finished_) {
            callback = CompleteLocked(playback_id);
        } else if (stream_started_) {
            underrun_count = ++underrun_count_;
        }
    }
    if (underrun_count != 0) {
        ESP_LOGW(TAG, "Notification playback underrun #%lu",
                 static_cast<unsigned long>(underrun_count));
    }
    if (callback) {
        callback(playback_id, true);
    }
}

// 参考 radio: HTTP 读超时 10s（vendor IO 硬编码 30s，太长导致 Stop 等待过久）
constexpr int kHttpReadTimeoutMs = 10 * 1000;

int NotifyPlayer::GmfHttpEventCb(http_stream_event_msg_t* msg) {
    auto* self = static_cast<NotifyPlayer*>(msg->user_data);
    if (msg != nullptr && msg->event_id == HTTP_STREAM_PRE_REQUEST && self != nullptr) {
        esp_http_client_handle_t client = static_cast<esp_http_client_handle_t>(msg->http_client);
        if (client != nullptr) {
            std::string username;
            std::string password;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                username = self->username_;
                password = self->password_;
            }
            if (!username.empty()) {
                esp_http_client_set_username(client, username.c_str());
                esp_http_client_set_password(client, password.c_str());
                esp_http_client_set_authtype(client, HTTP_AUTH_TYPE_BASIC);
            }
            // 参考 radio: 收窄读超时到 10s，设置 TCP keepalive 避免长连接假死
            esp_http_client_set_timeout_ms(client, kHttpReadTimeoutMs);
            int fd = esp_http_client_get_socket(client);
            if (fd >= 0) {
                int one = 1;
                setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
                int idle = 20, intvl = 10, cnt = 3;
                setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
                setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
            }
        }
    }
    return ESP_GMF_ERR_OK;
}

esp_gmf_err_t NotifyPlayer::GmfEventFunc(esp_gmf_event_pkt_t* pkt, void* ctx) {
    return static_cast<NotifyPlayer*>(ctx)->GmfEventCb(pkt, ctx);
}

esp_gmf_err_t NotifyPlayer::GmfEventCb(esp_gmf_event_pkt_t* pkt, void* ctx) {
    auto* self = static_cast<NotifyPlayer*>(ctx);
    if (pkt == nullptr) {
        return ESP_GMF_ERR_OK;
    }
    if (pkt->type == ESP_GMF_EVT_TYPE_CHANGE_STATE) {
        switch (pkt->sub) {
            case ESP_GMF_EVENT_STATE_RUNNING: {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->gmf_state_ = ESP_GMF_EVENT_STATE_RUNNING;
                break;
            }
            case ESP_GMF_EVENT_STATE_PAUSED: {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->gmf_state_ = ESP_GMF_EVENT_STATE_PAUSED;
                break;
            }
            case ESP_GMF_EVENT_STATE_STOPPED: {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->gmf_state_ = ESP_GMF_EVENT_STATE_STOPPED;
                break;
            }
            case ESP_GMF_EVENT_STATE_FINISHED: {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->gmf_state_ = ESP_GMF_EVENT_STATE_FINISHED;
                break;
            }
            case ESP_GMF_EVENT_STATE_ERROR: {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->gmf_state_ = ESP_GMF_EVENT_STATE_ERROR;
                break;
            }
            default:
                break;
        }
        ESP_LOGI(TAG, "GMF state: %s",
                 esp_gmf_event_get_state_str(static_cast<esp_gmf_event_state_t>(pkt->sub)));
    } else if (pkt->type == ESP_GMF_EVT_TYPE_REPORT_INFO) {
        if (pkt->payload_size > 0) {
            esp_gmf_info_sound_t info = {};
            memcpy(&info, pkt->payload, pkt->payload_size);
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->gmf_sample_rate_ = info.sample_rates;
            self->gmf_channels_ = info.channels;
            self->gmf_bits_ = info.bits;
        }
    }
    return ESP_GMF_ERR_OK;
}

int NotifyPlayer::GmfOutCb(uint8_t* data, int data_size, void* ctx) {
    auto* self = static_cast<NotifyPlayer*>(ctx);
    if (data_size <= 0) {
        return data_size;
    }
    const unsigned int bits_per_sample =
        self->gmf_bits_ > 0 ? static_cast<unsigned int>(self->gmf_bits_) : 16u;
    const size_t bytes_per_sample = bits_per_sample / 8;
    const size_t channels = self->gmf_channels_ > 1 ? static_cast<size_t>(self->gmf_channels_) : 1;
    const size_t mono_samples = (data_size / bytes_per_sample) / channels;
    std::vector<int16_t> pcm;
    pcm.reserve(mono_samples);
    for (size_t i = 0; i < mono_samples; ++i) {
        size_t base = i * channels * bytes_per_sample;
        uint32_t sum = 0;
        uint32_t count = 0;
        for (size_t c = 0; c < channels; ++c, base += bytes_per_sample) {
            sum += static_cast<uint32_t>(data[base]) | (static_cast<uint32_t>(data[base + 1]) << 8);
            ++count;
        }
        pcm.push_back(
            static_cast<int16_t>(static_cast<int32_t>(sum / static_cast<int32_t>(count))));
    }
    if (!pcm.empty()) {
        // 累计已播位置：data_size 为解码输出的 PCM 字节数
        self->gmf_playback_pos_ms_ +=
            static_cast<uint32_t>(data_size * 1000 / (bytes_per_sample * channels * self->gmf_sample_rate_));
        self->audio_service_.PushPcmPlaybackTask(std::move(pcm), self->gmf_sample_rate_, 1,
                                                 self->playback_id_, self->gmf_playback_pos_ms_);
        // CBR 估算时长：已播放字节数 / 已播放时间
        esp_gmf_io_handle_t in_io = nullptr;
        esp_gmf_err_t io_ret = ESP_GMF_ERR_NOT_SUPPORT;
        if (self->gmf_pipeline_ != nullptr) {
            io_ret = esp_gmf_pipeline_get_in(self->gmf_pipeline_, &in_io);
        }
        esp_gmf_info_file_t finfo = {};
        if (io_ret == ESP_GMF_ERR_OK && esp_gmf_io_get_info(in_io, &finfo) == ESP_GMF_ERR_OK && finfo.size > 0) {
            double elapsed_sec = static_cast<double>(self->gmf_playback_pos_ms_) / 1000.0;
            if (elapsed_sec > 3.0 && self->gmf_duration_ms_ == 0) {  // 等待 3s 且未锁定时才计算
                double bitrate_bps = (static_cast<double>(finfo.pos) * 8.0) / elapsed_sec;
                if (bitrate_bps > 0.0) {
                    // duration_ms = size(字节) * 8 / bitrate(bps) * 1000
                    self->gmf_duration_ms_ = static_cast<uint32_t>(static_cast<double>(finfo.size) * 8.0 * 1000.0 / bitrate_bps);
                    ESP_LOGI(TAG, "GMF duration locked: %u ms (bps=%.0f)", self->gmf_duration_ms_, bitrate_bps);
                }
            }
        }
    }
    return data_size;
}

esp_gmf_err_io_t NotifyPlayer::GmfOutPortAcquire(void* handle, esp_gmf_payload_t* load,
                                                 uint32_t wanted_size, int wait_ticks) {
    (void)handle;
    (void)load;
    (void)wanted_size;
    (void)wait_ticks;
    return ESP_GMF_IO_OK;
}

esp_gmf_err_io_t NotifyPlayer::GmfOutPortRelease(void* handle, esp_gmf_payload_t* load,
                                                 int wait_ticks) {
    (void)wait_ticks;
    auto* self = static_cast<NotifyPlayer*>(handle);
    if (load != nullptr && load->valid_size > 0) {
        self->GmfOutCb(load->buf, load->valid_size, handle);
    }
    return ESP_GMF_IO_OK;
}

bool NotifyPlayer::StartGmfPlayback(const std::string& audio_url, uint32_t playback_id, uint64_t start_byte_pos,
                                    uint32_t start_pos_ms, uint32_t start_duration_ms) {
    // 清理上一轮残留的 GMF 资源（切歌时 WorkerTask 仅置标志，此处兜底清理）
    StopGmfPlayback();

    esp_gmf_pool_handle_t pool = nullptr;
    if (esp_gmf_pool_init(&pool) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to init GMF pool");
        return false;
    }
    gmf_pool_ = pool;

    const char* el_names[4] = {"aud_dec"};
    int num_el_names = 1;
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    el_names[num_el_names++] = "aud_rate_cvt";
#endif
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_CH_CVT_EN
    el_names[num_el_names++] = "aud_ch_cvt";
#endif
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_BIT_CVT_EN
    el_names[num_el_names++] = "aud_bit_cvt";
#endif

    esp_gmf_element_handle_t el = nullptr;
    if (s_audio_dec_reg_ref_count == 0) {
        if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Failed to register default audio decoders");
            StopGmfPlayback();
            return false;
        }
        esp_audio_simple_dec_register_default();
    }
    ++s_audio_dec_reg_ref_count;
    esp_audio_simple_dec_cfg_t dec_cfg = {};
    dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    dec_cfg.dec_cfg = NULL;
    dec_cfg.cfg_size = 0;
    dec_cfg.use_frame_dec = false;
    if (esp_gmf_audio_dec_init(&dec_cfg, &el) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to init GMF audio decoder");
        StopGmfPlayback();
        return false;
    }
    if (esp_gmf_pool_register_element(gmf_pool_, el, NULL) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to register GMF audio decoder");
        StopGmfPlayback();
        return false;
    }
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_RESAMPLE_EN
    {
        esp_ae_rate_cvt_cfg_t rate_cvt_cfg = {};
        rate_cvt_cfg.src_rate = 44100;
        rate_cvt_cfg.dest_rate = CONFIG_AUDIO_SIMPLE_PLAYER_RESAMPLE_DEST_RATE;
        rate_cvt_cfg.channel = 2;
        rate_cvt_cfg.bits_per_sample = 16;
        rate_cvt_cfg.complexity = 2;
        rate_cvt_cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
        esp_gmf_element_handle_t rate_el = nullptr;
        if (esp_gmf_rate_cvt_init(&rate_cvt_cfg, &rate_el) != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "Failed to init GMF rate convert");
            StopGmfPlayback();
            return false;
        }
        if (esp_gmf_pool_register_element(gmf_pool_, rate_el, NULL) != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "Failed to register GMF rate convert");
            StopGmfPlayback();
            return false;
        }
    }
#endif
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_CH_CVT_EN
    {
        esp_ae_ch_cvt_cfg_t ch_cvt_cfg = {};
        ch_cvt_cfg.sample_rate = 48000;
        ch_cvt_cfg.bits_per_sample = 16;
        ch_cvt_cfg.src_ch = 2;
        ch_cvt_cfg.dest_ch = CONFIG_AUDIO_SIMPLE_PLAYER_CH_CVT_DEST;
        ch_cvt_cfg.weight = NULL;
        ch_cvt_cfg.weight_len = 0;
        esp_gmf_element_handle_t ch_el = nullptr;
        if (esp_gmf_ch_cvt_init(&ch_cvt_cfg, &ch_el) != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "Failed to init GMF channel convert");
            StopGmfPlayback();
            return false;
        }
        if (esp_gmf_pool_register_element(gmf_pool_, ch_el, NULL) != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "Failed to register GMF channel convert");
            StopGmfPlayback();
            return false;
        }
    }
#endif
#ifdef CONFIG_ESP_AUDIO_SIMPLE_PLAYER_BIT_CVT_EN
    {
        esp_ae_bit_cvt_cfg_t bit_cvt_cfg = {};
        bit_cvt_cfg.sample_rate = 48000;
        bit_cvt_cfg.channel = 2;
        bit_cvt_cfg.src_bits = 16;
        bit_cvt_cfg.dest_bits = 16;
        esp_gmf_element_handle_t bit_el = nullptr;
        if (esp_gmf_bit_cvt_init(&bit_cvt_cfg, &bit_el) != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "Failed to init GMF bit convert");
            StopGmfPlayback();
            return false;
        }
        if (esp_gmf_pool_register_element(gmf_pool_, bit_el, NULL) != ESP_GMF_ERR_OK) {
            ESP_LOGE(TAG, "Failed to register GMF bit convert");
            StopGmfPlayback();
            return false;
        }
    }
#endif

    // 仅注册 HTTP IO 到 pool（参考 radio），带 Basic Auth + keepalive + 10s 读超时
    // 关键：HTTP IO ring buffer 调大（radio HLS 用同规格），吸收网络抖动防止 underrun
    http_io_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
    http_cfg.dir = ESP_GMF_IO_DIR_READER;
    http_cfg.user_data = this;
    http_cfg.event_handle = &NotifyPlayer::GmfHttpEventCb;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.io_cfg.thread.stack = 24 * 1024;
    http_cfg.io_cfg.thread.prio = 5;
    http_cfg.io_cfg.buffer_cfg.buffer_size =
        2 * 1024 * 1024;  // 2MB PSRAM ring buffer (radio-aligned)

    esp_gmf_io_handle_t http_io = nullptr;
    if (esp_gmf_io_http_init(&http_cfg, &http_io) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to init HTTP IO");
        StopGmfPlayback();
        return false;
    }
    // 设置起始播放位置（在 pipeline 创建前，清理重置之后）
    if (start_pos_ms > 0) {
        gmf_playback_pos_ms_ = start_pos_ms;
        gmf_duration_ms_ = start_duration_ms;
    }
    if (esp_gmf_pool_register_io(gmf_pool_, http_io, NULL) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to register HTTP IO to pool");
        esp_gmf_io_deinit(http_io);
        StopGmfPlayback();
        return false;
    }

    char* in_str = nullptr;
    if (esp_gmf_pool_get_io_tag_by_url(gmf_pool_, audio_url.c_str(), ESP_GMF_IO_DIR_READER,
                                       &in_str) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to get IO tag for %s", audio_url.c_str());
        StopGmfPlayback();
        return false;
    }

    esp_gmf_pipeline_handle_t pipe = nullptr;
    esp_gmf_err_t ret =
        esp_gmf_pool_new_pipeline(gmf_pool_, in_str, el_names, num_el_names, NULL, &pipe);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to create GMF pipeline, ret:%d", ret);
        StopGmfPlayback();
        return false;
    }
    gmf_pipeline_ = pipe;

    esp_gmf_port_handle_t out_port =
        reinterpret_cast<esp_gmf_port_handle_t>(NEW_ESP_GMF_PORT_OUT_BYTE(
            reinterpret_cast<void*>(GmfOutPortAcquire), reinterpret_cast<void*>(GmfOutPortRelease),
            nullptr, this, 2048, ESP_GMF_MAX_DELAY));
    if (out_port == nullptr) {
        ESP_LOGE(TAG, "Failed to create GMF out port");
        StopGmfPlayback();
        return false;
    }
    gmf_http_io_ = http_io;
    // 缓存文件大小用于 seek 计算
    {
        esp_gmf_info_file_t info = {};
        if (esp_gmf_io_get_info(gmf_http_io_, &info) == ESP_GMF_ERR_OK) {
            gmf_file_size_ = info.size;
        }
    }
    ret = esp_gmf_pipeline_reg_el_port(
        gmf_pipeline_, OBJ_GET_TAG(ESP_GMF_PIPELINE_GET_LAST_ELEMENT(gmf_pipeline_)),
        ESP_GMF_IO_DIR_WRITER, out_port);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to register GMF out port, ret:%d", ret);
        StopGmfPlayback();
        return false;
    }

    esp_gmf_task_cfg_t task_cfg = DEFAULT_ESP_GMF_TASK_CONFIG();
    task_cfg.thread.stack = 12 * 1024;
    task_cfg.thread.prio = 6;
    task_cfg.thread.core = 0;
    // 栈放 PSRAM：内部 SRAM 紧张时 12KB 连续块分配失败会导致播放起不来
    task_cfg.thread.stack_in_ext = true;
    task_cfg.name = "audio";
    if (esp_gmf_task_init(&task_cfg, &gmf_task_) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to init GMF task");
        StopGmfPlayback();
        return false;
    }
    ret = esp_gmf_task_set_timeout(gmf_task_, 5000);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to set GMF task timeout, ret:%d", ret);
        StopGmfPlayback();
        return false;
    }
    ret = esp_gmf_pipeline_bind_task(gmf_pipeline_, gmf_task_);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to bind GMF task, ret:%d", ret);
        StopGmfPlayback();
        return false;
    }

    esp_gmf_element_handle_t dec_el = nullptr;
    ret = esp_gmf_pipeline_get_el_by_name(gmf_pipeline_, "aud_dec", &dec_el);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to get GMF decoder element, ret:%d", ret);
        StopGmfPlayback();
        return false;
    }
    esp_gmf_info_sound_t info = {};
    info.sample_rates = 16000;
    info.channels = 1;
    info.bits = 16;
    info.bitrate = 0;
    esp_gmf_audio_helper_get_audio_type_by_uri(audio_url.c_str(), &info.format_id);
    if (info.format_id == 0) {
        info.format_id = ESP_FOURCC_MP3;
    }
    ret = esp_gmf_audio_dec_reconfig_by_sound_info(dec_el, &info);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to reconfig GMF decoder, ret:%d", ret);
        StopGmfPlayback();
        return false;
    }
    ret = esp_gmf_pipeline_set_in_uri(gmf_pipeline_, audio_url.c_str());
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to set GMF in uri, ret:%d", ret);
        StopGmfPlayback();
        return false;
    }
    // pipeline 里的 IO 是 pool 复制出来的对象，必须设在它真正使用的那个 IO 上，
    // 否则 Range 头不会被写入 HTTP 请求，服务器返回 200 导致从开头播放。
    if (start_byte_pos > 0) {
        esp_gmf_io_handle_t pipeline_in = nullptr;
        if (esp_gmf_pipeline_get_in(gmf_pipeline_, &pipeline_in) == ESP_GMF_ERR_OK &&
            pipeline_in != nullptr) {
            if (esp_gmf_io_set_pos(pipeline_in, start_byte_pos) == ESP_GMF_ERR_OK) {
                ESP_LOGI(TAG, "Set HTTP IO start position: %llu", (unsigned long long)start_byte_pos);
            }
        }
    }
    ret = esp_gmf_pipeline_loading_jobs(gmf_pipeline_);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to load GMF jobs, ret:%d", ret);
        StopGmfPlayback();
        return false;
    }
    ret = esp_gmf_pipeline_set_event(gmf_pipeline_, &NotifyPlayer::GmfEventFunc, this);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Failed to set GMF pipeline event, ret:%d", ret);
        StopGmfPlayback();
        return false;
    }

    ret = esp_gmf_pipeline_run(gmf_pipeline_);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "GMF pipeline run failed: %d", ret);
        StopGmfPlayback();
        return false;
    }
    return WaitForGmfFinish(playback_id);
}

bool NotifyPlayer::WaitForGmfFinish(uint32_t session_playback_id) {
    esp_gmf_event_state_t state = ESP_GMF_EVENT_STATE_NONE;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_ || !active_ || playback_id_ != session_playback_id) {
                return false;
            }
            state = gmf_state_;
        }
        if (state == ESP_GMF_EVENT_STATE_FINISHED) {
            return true;
        }
        if (state == ESP_GMF_EVENT_STATE_STOPPED || state == ESP_GMF_EVENT_STATE_ERROR) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool NotifyPlayer::GetPlaybackInfo(PlaybackInfo& info) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_ || gmf_pipeline_ == nullptr) {
        return false;
    }
    info.playback_id = playback_id_;
    info.duration_ms = gmf_duration_ms_;
    info.position_ms = gmf_playback_pos_ms_;
    info.file_size = gmf_file_size_;
    esp_gmf_io_handle_t in_io = nullptr;
    if (esp_gmf_pipeline_get_in(gmf_pipeline_, &in_io) == ESP_GMF_ERR_OK && in_io != nullptr) {
        esp_gmf_info_file_t file_info = {};
        if (esp_gmf_io_get_info(in_io, &file_info) == ESP_GMF_ERR_OK) {
            info.byte_pos = file_info.pos;
            info.file_size = file_info.size;
        }
    }
    return true;
}

bool NotifyPlayer::Seek(uint32_t time_ms) {
    bool paused = false;
    std::string audio_url;
    std::string username;
    std::string password;
    std::vector<NotifySubtitle> subtitles;
    SubtitleCallback subtitle_callback;
    FinishedCallback finished_callback;
    uint32_t playback_id = 0;
    uint32_t duration_ms = 0;
    uint64_t file_size = 0;
    esp_gmf_pipeline_handle_t pipeline = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        paused = gmf_pipeline_ != nullptr && gmf_state_ == ESP_GMF_EVENT_STATE_PAUSED;
        pipeline = gmf_pipeline_;
        if (gmf_pipeline_ == nullptr) {
            return false;
        }
        audio_url = audio_url_;
        username = username_;
        password = password_;
        subtitles = subtitles_;
        subtitle_callback = subtitle_callback_;
        finished_callback = finished_callback_;
        playback_id = playback_id_;
        duration_ms = gmf_duration_ms_;
    }
    {
        esp_gmf_io_handle_t in_io = nullptr;
        esp_gmf_err_t in_ret = ESP_GMF_ERR_NOT_SUPPORT;
        if (pipeline != nullptr) {
            in_ret = esp_gmf_pipeline_get_in(pipeline, &in_io);
        }
        if (in_ret == ESP_GMF_ERR_OK && in_io != nullptr) {
            esp_gmf_info_file_t info = {};
            if (esp_gmf_io_get_info(in_io, &info) == ESP_GMF_ERR_OK) {
                file_size = info.size;
            }
        }
    }
    ESP_LOGI(TAG, "Seek called: target=%u ms, paused=%d, duration=%u, file_size=%llu", time_ms,
             paused ? 1 : 0, duration_ms, (unsigned long long)file_size);
    if (paused) {
        // 暂停态：GMF 支持在 PAUSED 状态直接 seek
        if (duration_ms == 0 || file_size == 0) {
            ESP_LOGW(TAG, "Seek aborted: no duration/file_size");
            return false;
        }
        double duration_sec = static_cast<double>(duration_ms) / 1000.0;
        uint64_t byte_pos = static_cast<uint64_t>(static_cast<double>(time_ms) / 1000.0 / duration_sec * static_cast<double>(file_size));
        esp_gmf_err_t ret = esp_gmf_pipeline_seek(pipeline, byte_pos);
        if (ret == ESP_GMF_ERR_OK) {
            esp_gmf_obj_t* obj = (esp_gmf_obj_t*)pipeline->head_el;
            while (obj != nullptr) {
                esp_gmf_element_reset_port((esp_gmf_element_handle_t)obj);
                obj = obj->next;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                gmf_playback_pos_ms_ = time_ms;
            }
        }
        ESP_LOGI(TAG, "Seek OK (paused): target=%u ms, byte_pos=%llu, ret=%d", time_ms, (unsigned long long)byte_pos, ret);
        return ret == ESP_GMF_ERR_OK;
    }
    // 播放中（或状态未知）：通过重新开始播放实现 seek，避免异步 seek 导致的端口重置问题
    return SeekAndRestart(time_ms);
}

bool NotifyPlayer::SeekAndRestart(uint32_t time_ms) {
    ESP_LOGI(TAG, "SeekAndRestart: target=%u ms", time_ms);
    std::string audio_url;
    std::string username;
    std::string password;
    std::vector<NotifySubtitle> subtitles;
    SubtitleCallback subtitle_callback;
    FinishedCallback finished_callback;
    uint32_t playback_id = 0;
    uint32_t duration_ms = 0;
    uint64_t file_size = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_) {
            return false;
        }
        audio_url = audio_url_;
        username = username_;
        password = password_;
        subtitles = subtitles_;
        subtitle_callback = subtitle_callback_;
        finished_callback = finished_callback_;
        playback_id = playback_id_;
        duration_ms = gmf_duration_ms_;
    }
    {
        esp_gmf_io_handle_t in_io = nullptr;
        esp_gmf_err_t in_ret = ESP_GMF_ERR_NOT_SUPPORT;
        if (gmf_http_io_ != nullptr) {
            in_ret = esp_gmf_pipeline_get_in(gmf_pipeline_, &in_io);
        }
        if (in_ret == ESP_GMF_ERR_OK && in_io != nullptr) {
            esp_gmf_info_file_t info = {};
            if (esp_gmf_io_get_info(in_io, &info) == ESP_GMF_ERR_OK) {
                file_size = info.size;
            }
        }
    }
    if (audio_url.empty() || duration_ms == 0) {
        return false;
    }
    double duration_sec = static_cast<double>(duration_ms) / 1000.0;
    uint64_t byte_pos = 0;
    if (duration_ms > 0 && file_size > 0) {
        byte_pos = static_cast<uint64_t>(static_cast<double>(time_ms) / 1000.0 / duration_sec * static_cast<double>(file_size));
    } else {
        uint64_t pos_now = 0;
        uint32_t pos_ms = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pos_ms = gmf_playback_pos_ms_;
            if (gmf_http_io_ != nullptr) {
                esp_gmf_info_file_t info = {};
                if (esp_gmf_io_get_info(gmf_http_io_, &info) == ESP_GMF_ERR_OK) {
                    pos_now = info.pos;
                }
            }
        }
        double elapsed_sec = static_cast<double>(pos_ms) / 1000.0;
        if (elapsed_sec > 0.0 && pos_now > 0) {
            byte_pos = static_cast<uint64_t>(static_cast<double>(time_ms) / 1000.0 * pos_now / elapsed_sec);
        }
    }
    ESP_LOGI(TAG, "SeekAndRestart calc: target=%u ms, byte_pos=%llu, duration=%u ms", time_ms, (unsigned long long)byte_pos, duration_ms);

    Stop();

    uint32_t new_playback_id = playback_id + 1;
    if (new_playback_id == 0) {
        new_playback_id = 1;
    }
    bool ok = Start(audio_url, subtitles, new_playback_id, subtitle_callback, finished_callback, username, password, byte_pos, time_ms, duration_ms);
    return ok;
}

void NotifyPlayer::StopGmfPlayback() {
    esp_gmf_pipeline_handle_t pipeline = gmf_pipeline_;
    esp_gmf_task_handle_t task = gmf_task_;
    esp_gmf_pool_handle_t pool = gmf_pool_;
    gmf_pipeline_ = nullptr;
    gmf_task_ = nullptr;
    gmf_pool_ = nullptr;
    gmf_http_io_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gmf_duration_ms_ = 0;
        gmf_playback_pos_ms_ = 0;
        gmf_file_size_ = 0;
    }
    if (pipeline == nullptr) {
        return;
    }
    esp_gmf_err_t ret = esp_gmf_pipeline_stop(pipeline);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGW(TAG, "GMF pipeline stop ret:%d", ret);
    }
    if (task != nullptr) {
        esp_gmf_task_deinit(task);
    }
    esp_gmf_pipeline_destroy(pipeline);
    if (pool != nullptr) {
        esp_gmf_pool_deinit(pool);
    }
    // 与 ASP 一致：释放默认解码器注册（refcount 归零时注销）
    if (s_audio_dec_reg_ref_count > 0) {
        --s_audio_dec_reg_ref_count;
        if (s_audio_dec_reg_ref_count == 0) {
            esp_audio_dec_unregister_default();
        }
    }
}

void NotifyPlayer::WorkerEntry(void* arg) {
    auto* player = static_cast<NotifyPlayer*>(arg);
    player->WorkerTask();
    vTaskDelete(nullptr);
}

void NotifyPlayer::WorkerTask() {
    std::string audio_url;
    uint32_t playback_id = 0;
    uint64_t start_byte_pos = 0;
    uint32_t start_pos_ms = 0;
    uint32_t start_duration_ms = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        audio_url = audio_url_;
        playback_id = playback_id_;
        start_byte_pos = start_byte_pos_;
        start_pos_ms = start_pos_ms_;
        start_duration_ms = start_duration_ms_;
    }

    bool success = StartGmfPlayback(audio_url, playback_id, start_byte_pos, start_pos_ms, start_duration_ms);

    FinishedCallback callback;
    uint32_t completed_playback_id = 0;
    bool report_success = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        task_handle_ = nullptr;
        if (!cancelled_ && active_ && playback_id == playback_id_) {
            if (!success) {
                callback = CompleteLocked(completed_playback_id);
            } else {
                http_finished_ = true;
                if (playback_drained_) {
                    callback = CompleteLocked(completed_playback_id);
                    report_success = true;
                }
            }
        }
    }
    if (callback) {
        callback(completed_playback_id, report_success);
    }
}
