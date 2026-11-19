#pragma once

#include <functional>
#include <queue>
#include <vector>
#include <mutex>
#include <memory>
#include <string>
#include <optional>
#include <chrono>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "notify/notify_player.h"
#include "application.h"

enum class PlaybackPriority : uint8_t {
    SYSTEM = 0,      // 最高：系统关键播放（如启动音、报警）
    TTS = 10,        // 高：AI 语音回复
    MUSIC = 20,      // 中：音乐播放
    NOTIFICATION = 30, // 低：通知提示音
    IDLE = 255       // 空闲
};

enum class PlaybackCommandType : uint8_t {
    PLAY,           // 开始播放（含 URL、标题、凭证）
    PAUSE,          // 暂停
    RESUME,         // 恢复
    STOP,           // 停止
    NEXT,           // 下一首
    PREV,           // 上一首
    SEEK,           // 跳转
    VOLUME,         // 音量
    MODE,           // 播放模式
    TTS_PLAY,       // TTS 播放（二进制流）
    TTS_STOP        // TTS 停止
};

// 播放命令执行结果（独立于 MCP，避免头文件循环依赖）
struct PlaybackResult {
    bool success = false;
    std::string error;
};

// 提交命令的同步等待句柄：持有信号量和结果，命令销毁后仍可安全读取
struct PlaybackWaitHandle {
    SemaphoreHandle_t sem = nullptr;
    PlaybackResult result;
    ~PlaybackWaitHandle() { if (sem) vSemaphoreDelete(sem); }
    bool Wait(int timeout_ms) {
        if (!sem) return false;
        return xSemaphoreTake(sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    }
};
using PlaybackWaitHandlePtr = std::shared_ptr<PlaybackWaitHandle>;

struct PlaybackCommand {
    PlaybackCommandType type;
    PlaybackPriority priority;
    
    // 通用字段
    std::string audio_url;
    std::string title;
    std::string username;
    std::string password;
    
    // 播放控制
    uint32_t seek_ms = 0;
    int volume = -1;
    Application::MusicPlayMode mode = Application::MusicPlayMode::kSequential;
    int track_index = -1;
    int source_index = -1;
    std::vector<Application::MusicSource> playlist;
    bool random = false;
    bool save_state = true;  // 被打断时是否保存状态用于恢复
    
    // 同步等待句柄（外部持有，安全）
    PlaybackWaitHandlePtr wait_handle;
    
    // 内部状态
    bool interrupted = false;  // 是否被打断
    std::chrono::steady_clock::time_point submit_time;
    
    // 完成通知（委托给外部持有的句柄）
    void SignalDone(bool success, const std::string& error = "") {
        wait_handle->result.success = success;
        wait_handle->result.error = error;
        if (wait_handle->sem) xSemaphoreGive(wait_handle->sem);
    }
};

class PlaybackScheduler {
public:
    static PlaybackScheduler& GetInstance() {
        static PlaybackScheduler instance;
        return instance;
    }
    
    // 禁止拷贝
    PlaybackScheduler(const PlaybackScheduler&) = delete;
    PlaybackScheduler& operator=(const PlaybackScheduler&) = delete;
    
    // 初始化（需在 Application 初始化后调用）
    void Initialize(Application& app);
    
    // 提交播放命令，返回是否入队成功
    bool Submit(std::unique_ptr<PlaybackCommand> cmd);
    
    // 提交命令并等待执行完成（同步），返回执行结果
    PlaybackResult SubmitAndWait(std::unique_ptr<PlaybackCommand> cmd, int timeout_ms);
    
    // 获取当前播放状态
    struct Status {
        bool is_playing = false;
        PlaybackPriority current_priority = PlaybackPriority::IDLE;
        std::string current_title;
        uint32_t position_ms = 0;
        uint32_t duration_ms = 0;
        int queue_size = 0;
    };
    Status GetStatus() const;
    
    // 清空队列（保留当前播放）
    void ClearQueue(bool stop_current = false);
    
    // 获取调度器任务句柄（用于监控）
    TaskHandle_t GetTaskHandle() const { return task_handle_; }

public:
    PlaybackScheduler();
    ~PlaybackScheduler();

private:
    // 调度器主任务
    static void TaskEntry(void* arg);
    void TaskLoop();
    void Enqueue(std::unique_ptr<PlaybackCommand> cmd);
    void ExecuteCommand(std::unique_ptr<PlaybackCommand> cmd);
    void FinishCurrent();
    
    // 处理命令
    void HandlePlayCommand(PlaybackCommand* cmd);
    void HandlePauseCommand(PlaybackCommand* cmd);
    void HandleResumeCommand(PlaybackCommand* cmd);
    void HandleStopCommand(PlaybackCommand* cmd);
    void HandleNextCommand(PlaybackCommand* cmd);
    void HandlePrevCommand(PlaybackCommand* cmd);
    void HandleSeekCommand(PlaybackCommand* cmd);
    void HandleVolumeCommand(PlaybackCommand* cmd);
    void HandleModeCommand(PlaybackCommand* cmd);
    void HandleTTSPlayCommand(PlaybackCommand* cmd);
    void HandleTTSStopCommand(PlaybackCommand* cmd);
    
    // 成员变量
    Application* app_ = nullptr;
    NotifyPlayer* notify_player_ = nullptr;
    
    // 优先级队列比较器（优先级值小者先出队）
    struct CmdCompare {
        bool operator()(const std::unique_ptr<PlaybackCommand>& a,
                        const std::unique_ptr<PlaybackCommand>& b) const {
            if (a->priority != b->priority) {
                return static_cast<uint8_t>(a->priority) > static_cast<uint8_t>(b->priority);
            }
            return a->submit_time > b->submit_time;
        }
    };
    mutable std::mutex queue_mutex_;
    std::priority_queue<std::unique_ptr<PlaybackCommand>,
                        std::vector<std::unique_ptr<PlaybackCommand>>,
                        CmdCompare> pending_queue_;
    
    // 当前执行（单任务串行处理，命令处理完即释放）
    std::unique_ptr<PlaybackCommand> current_cmd_;
    
    // 调度器任务
    TaskHandle_t task_handle_ = nullptr;
    bool running_ = false;
    SemaphoreHandle_t wake_sem_ = nullptr;
    
    // 统计
    mutable std::mutex status_mutex_;
    struct {
        bool is_playing = false;
        PlaybackPriority priority = PlaybackPriority::IDLE;
        std::string title;
        uint32_t position_ms = 0;
        uint32_t duration_ms = 0;
    } status_;
};