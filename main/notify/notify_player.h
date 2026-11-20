#ifndef NOTIFY_PLAYER_H_
#define NOTIFY_PLAYER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "audio_service.h"
#include "esp_gmf_audio_dec.h"
#include "esp_gmf_audio_element.h"
#include "esp_gmf_audio_helper.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_err.h"
#include "esp_gmf_event.h"
#include "esp_gmf_io.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_pipeline.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_task.h"

struct NotifySubtitle {
    uint32_t start_ms = 0;
    std::string text;
};

class NotifyPlayer {
public:
    using SubtitleCallback = std::function<void(uint32_t playback_id, const std::string& text)>;
    using FinishedCallback = std::function<void(uint32_t playback_id, bool success)>;

    explicit NotifyPlayer(AudioService& audio_service);
    ~NotifyPlayer();

    bool Start(std::string audio_url, std::vector<NotifySubtitle> subtitles, uint32_t playback_id,
               SubtitleCallback subtitle_callback, FinishedCallback finished_callback,
               const std::string& username = "", const std::string& password = "",
               uint64_t start_byte_pos = 0, uint32_t start_pos_ms = 0, uint32_t start_duration_ms = 0);
    void Stop();
    // 暂停时读取真实 IO 信息，供上层保存快照（不在此处持有快照）
    struct PlaybackInfo {
        uint64_t byte_pos = 0;
        uint64_t file_size = 0;
        uint32_t duration_ms = 0;
        uint32_t position_ms = 0;
        uint32_t playback_id = 0;
    };
    bool GetPlaybackInfo(PlaybackInfo& info) const;
    // 播放中断回调里 active_ 已被清，但管道/IO 仍存活；读取最后已知字节位置用于断点续播
    uint64_t GetLastBytePos() const;
    uint32_t GetLastPositionMs() const { return gmf_playback_pos_ms_.load(); }
    uint32_t GetLastDurationMs() const { return gmf_duration_ms_.load(); }
    uint64_t GetLastFileSize() const { return gmf_file_size_; }
    bool Seek(uint32_t time_ms);
    void OnPlaybackProgress(uint32_t playback_id, uint32_t media_position_ms);
    void OnPlaybackDrained();
    bool IsActive(uint32_t playback_id = 0) const;
    bool IsBusy() const;

    uint32_t GetGmfDurationMs() const { return gmf_duration_ms_; }

private:
    AudioService& audio_service_;
    // ESP-GMF playback pipeline handles (MP3 / WebDAV streams).
    esp_gmf_pool_handle_t gmf_pool_ = nullptr;
    esp_gmf_pipeline_handle_t gmf_pipeline_ = nullptr;
    esp_gmf_task_handle_t gmf_task_ = nullptr;
    esp_gmf_io_handle_t gmf_http_io_ = nullptr;
    uint64_t gmf_file_size_ = 0;
    esp_gmf_event_state_t gmf_state_ = ESP_GMF_EVENT_STATE_NONE;
    int gmf_sample_rate_ = 0;
    int gmf_channels_ = 1;
    uint16_t gmf_bits_ = 16;
    mutable std::mutex mutex_;
    std::string audio_url_;
    std::string username_;
    std::string password_;
    std::vector<NotifySubtitle> subtitles_;
    std::string displayed_text_;
    SubtitleCallback subtitle_callback_;
    FinishedCallback finished_callback_;
    TaskHandle_t task_handle_ = nullptr;
    uint32_t playback_id_ = 0;
    uint64_t start_byte_pos_ = 0;
    uint32_t start_pos_ms_ = 0;
    uint32_t start_duration_ms_ = 0;
    uint32_t underrun_count_ = 0;    size_t next_subtitle_index_ = 0;
    bool active_ = false;
    bool worker_running_ = false;
    std::atomic<uint32_t> gmf_playback_pos_ms_{0};
    std::atomic<uint32_t> gmf_duration_ms_{0};
    std::atomic<uint64_t> gmf_byte_pos_{0};   // 最后已知的 HTTP IO 字节位置（供中断续播）
    std::atomic<bool> gmf_duration_exact_{false};  // 时长由解码器精确上报，禁止 CBR 估算覆盖
    bool cancelled_ = false;
    bool http_finished_ = false;
    bool stream_started_ = false;
    bool playback_drained_ = false;
    bool completion_reported_ = false;

    static void WorkerEntry(void* arg);
    void WorkerTask();
    bool IsCancelled(uint32_t playback_id) const;
    FinishedCallback CompleteLocked(uint32_t& playback_id);

    // ESP-GMF playback path for streamable audio: drive the GMF pipeline directly.
    static esp_gmf_err_t GmfEventFunc(esp_gmf_event_pkt_t* pkt, void* ctx);
    static int GmfHttpEventCb(http_stream_event_msg_t* msg);
    static esp_gmf_err_io_t GmfOutPortAcquire(void* handle, esp_gmf_payload_t* load,
                                              uint32_t wanted_size, int wait_ticks);
    static esp_gmf_err_io_t GmfOutPortRelease(void* handle, esp_gmf_payload_t* load,
                                              int wait_ticks);
    bool StartGmfPlayback(const std::string& audio_url, uint32_t playback_id, uint64_t start_byte_pos = 0,
                           uint32_t start_pos_ms = 0, uint32_t start_duration_ms = 0);
    bool SeekAndRestart(uint32_t time_ms);
    bool WaitForGmfFinish(uint32_t playback_id);
    void StopGmfPlayback();
    int GmfOutCb(uint8_t* data, int data_size, void* ctx);
    esp_gmf_err_t GmfEventCb(esp_gmf_event_pkt_t* pkt, void* ctx);
};

#endif  // NOTIFY_PLAYER_H_
