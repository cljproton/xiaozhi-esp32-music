#include "playback_scheduler.h"
#include "application.h"
#include "board.h"
#include <esp_log.h>
#include <esp_random.h>

#define TAG "PlaybackScheduler"

PlaybackScheduler::PlaybackScheduler() = default;

PlaybackScheduler::~PlaybackScheduler() {
    running_ = false;
    if (wake_sem_) xSemaphoreGive(wake_sem_);
    if (task_handle_) {
        vTaskDelay(pdMS_TO_TICKS(20));
        vTaskDelete(task_handle_);
    }
    if (wake_sem_) vSemaphoreDelete(wake_sem_);
}

void PlaybackScheduler::Initialize(Application& app) {
    app_ = &app;
    notify_player_ = &app.GetNotifyPlayer();
    wake_sem_ = xSemaphoreCreateBinary();
    running_ = true;
    xTaskCreate(TaskEntry, "playback_sched", 4096, this, 5, &task_handle_);
    ESP_LOGI(TAG, "PlaybackScheduler initialized");
}

bool PlaybackScheduler::Submit(std::unique_ptr<PlaybackCommand> cmd) {
    if (!app_) return false;
    cmd->submit_time = std::chrono::steady_clock::now();
    Enqueue(std::move(cmd));
    return true;
}

PlaybackResult PlaybackScheduler::SubmitAndWait(std::unique_ptr<PlaybackCommand> cmd, int timeout_ms) {
    PlaybackResult failed;
    failed.success = false;
    if (!app_) { failed.error = "Scheduler not initialized"; return failed; }
    auto handle = std::make_shared<PlaybackWaitHandle>();
    handle->sem = xSemaphoreCreateBinary();
    if (!handle->sem) { failed.error = "Failed to create semaphore"; return failed; }
    cmd->wait_handle = handle;
    cmd->submit_time = std::chrono::steady_clock::now();
    Enqueue(std::move(cmd));
    if (!handle->Wait(timeout_ms)) { failed.error = "播放启动超时"; return failed; }
    return handle->result;
}

void PlaybackScheduler::Enqueue(std::unique_ptr<PlaybackCommand> cmd) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_queue_.push(std::move(cmd));
    }
    if (wake_sem_) xSemaphoreGive(wake_sem_);
}

void PlaybackScheduler::TaskEntry(void* arg) {
    static_cast<PlaybackScheduler*>(arg)->TaskLoop();
}

void PlaybackScheduler::TaskLoop() {
    ESP_LOGI(TAG, "PlaybackScheduler task started");
    while (running_) {
        std::unique_ptr<PlaybackCommand> cmd;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (!pending_queue_.empty()) {
                cmd = std::move(const_cast<std::unique_ptr<PlaybackCommand>&>(pending_queue_.top()));
                pending_queue_.pop();
            }
        }
        if (cmd) {
            ExecuteCommand(std::move(cmd));
        } else {
            if (wake_sem_) xSemaphoreTake(wake_sem_, pdMS_TO_TICKS(200));
        }
    }
    vTaskDelete(nullptr);
}

void PlaybackScheduler::ExecuteCommand(std::unique_ptr<PlaybackCommand> cmd) {
    current_cmd_ = std::move(cmd);
    ESP_LOGI(TAG, "Executing type=%d prio=%d title=%s",
             static_cast<int>(current_cmd_->type),
             static_cast<int>(current_cmd_->priority),
             current_cmd_->title.c_str());

    switch (current_cmd_->type) {
        case PlaybackCommandType::PLAY:          HandlePlayCommand(current_cmd_.get()); break;
        case PlaybackCommandType::PAUSE:         HandlePauseCommand(current_cmd_.get()); break;
        case PlaybackCommandType::RESUME:        HandleResumeCommand(current_cmd_.get()); break;
        case PlaybackCommandType::STOP:          HandleStopCommand(current_cmd_.get()); break;
        case PlaybackCommandType::NEXT:          HandleNextCommand(current_cmd_.get()); break;
        case PlaybackCommandType::PREV:          HandlePrevCommand(current_cmd_.get()); break;
        case PlaybackCommandType::SEEK:          HandleSeekCommand(current_cmd_.get()); break;
        case PlaybackCommandType::VOLUME:        HandleVolumeCommand(current_cmd_.get()); break;
        case PlaybackCommandType::MODE:          HandleModeCommand(current_cmd_.get()); break;
        case PlaybackCommandType::TTS_PLAY:      HandleTTSPlayCommand(current_cmd_.get()); break;
        case PlaybackCommandType::TTS_STOP:      HandleTTSStopCommand(current_cmd_.get()); break;
        default:
            current_cmd_->SignalDone(false, "Unknown command type");
            break;
    }

    // 所有命令都在本任务内同步完成（播放只是启动异步 pipeline），立即释放
    FinishCurrent();
}

void PlaybackScheduler::HandlePlayCommand(PlaybackCommand* cmd) {
    if (!cmd->playlist.empty()) {
        bool ok = app_->PlayMusicPlaylist(std::move(cmd->playlist), cmd->source_index,
                                          cmd->mode, cmd->track_index, 0);
        if (!ok) {
            cmd->SignalDone(false, "播放启动失败");
        } else {
            // 播放已异步启动，视为命令成功完成；实际播放由同一 pipeline 管理
            cmd->SignalDone(true);
        }
    } else {
        cmd->SignalDone(false, "No playlist");
    }
}

void PlaybackScheduler::HandlePauseCommand(PlaybackCommand* cmd) {
    bool ok = app_->PauseMusic();
    cmd->SignalDone(ok, ok ? "" : "Nothing playing");
}

void PlaybackScheduler::HandleResumeCommand(PlaybackCommand* cmd) {
    bool ok = app_->ResumeMusic();
    cmd->SignalDone(ok, ok ? "" : "Nothing paused");
}

void PlaybackScheduler::HandleStopCommand(PlaybackCommand* cmd) {
    app_->StopMusicPlayback();
    cmd->SignalDone(true);
}

void PlaybackScheduler::HandleNextCommand(PlaybackCommand* cmd) {
    cmd->SignalDone(app_->PlayNextMusic());
}

void PlaybackScheduler::HandlePrevCommand(PlaybackCommand* cmd) {
    cmd->SignalDone(app_->PlayPrevMusic());
}

void PlaybackScheduler::HandleSeekCommand(PlaybackCommand* cmd) {
    bool ok = (cmd->seek_ms > 0) && app_->SeekMusic(cmd->seek_ms);
    cmd->SignalDone(ok, ok ? "" : "Seek failed");
}

void PlaybackScheduler::HandleVolumeCommand(PlaybackCommand* cmd) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (codec && cmd->volume >= 0 && cmd->volume <= 100) {
        codec->SetOutputVolume(cmd->volume);
        cmd->SignalDone(true);
    } else {
        cmd->SignalDone(false, "Invalid volume");
    }
}

void PlaybackScheduler::HandleModeCommand(PlaybackCommand* cmd) {
    app_->SetMusicMode(cmd->mode);
    cmd->SignalDone(true);
}

void PlaybackScheduler::HandleTTSPlayCommand(PlaybackCommand* cmd) {
    // TTS 音频经 NotifyPlayer 播放（当前项目 TTS 走二进制流，此处仅保留接口）
    cmd->SignalDone(false, "TTS stream not routed via scheduler");
}

void PlaybackScheduler::HandleTTSStopCommand(PlaybackCommand* cmd) {
    if (notify_player_ && notify_player_->IsBusy()) notify_player_->Stop();
    cmd->SignalDone(true);
}

void PlaybackScheduler::FinishCurrent() {
    current_cmd_.reset();
    if (wake_sem_) xSemaphoreGive(wake_sem_);
}

void PlaybackScheduler::ClearQueue(bool stop_current) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!pending_queue_.empty()) {
        auto cmd = std::move(const_cast<std::unique_ptr<PlaybackCommand>&>(pending_queue_.top()));
        pending_queue_.pop();
        if (cmd->wait_handle) {
            cmd->SignalDone(false, "Cleared by user");
        }
    }
    if (stop_current && current_cmd_) {
        if (notify_player_) notify_player_->Stop();
        current_cmd_->SignalDone(false, "Stopped");
        FinishCurrent();
    }
}

PlaybackScheduler::Status PlaybackScheduler::GetStatus() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    Status s;
    s.is_playing = static_cast<bool>(current_cmd_);
    s.current_priority = current_cmd_ ? current_cmd_->priority : PlaybackPriority::IDLE;
    s.current_title = current_cmd_ ? current_cmd_->title : "";
    s.queue_size = pending_queue_.size();
    return s;
}
