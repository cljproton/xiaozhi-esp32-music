#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>
#include <cstdint>
#include <vector>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "notify/notify_player.h"

// Forward declaration
class PlaybackScheduler;
namespace radio { class RadioSource; }

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)
#define MAIN_EVENT_PLAYBACK_DRAINED     (1 << 13)
#define MAIN_EVENT_STARTUP_HINT_TIMEOUT (1 << 14)
#define MAIN_EVENT_PROTOCOL_PENDING (1 << 15)

enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    struct MusicSource {
        std::string audio_url;
        std::string username;
        std::string password;
        std::string title;
    };

    enum class MusicPlayMode {
        kSequential,
        kShuffle,
    };

    struct MusicStatus {
        bool active = false;
        bool paused = false;
        bool single = false;
        int source_index = -1;
        int track_index = -1;
        int total = 0;
        bool truncated = false;
        MusicPlayMode mode = MusicPlayMode::kSequential;
        std::string title;
        int position_ms = 0;
        int duration_ms = 0;
        int volume = 0;
    };

    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }

    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);
    void NotifyProtocolPending() { xEventGroupSetBits(event_group_, MAIN_EVENT_PROTOCOL_PENDING); }

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);
    // 播放前打断正在进行的 AI 语音，使 StartNotification 的 Idle 检查能够通过
    void InterruptSpeakingForPlayback();

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    /**
     * Start notification/music playback from an external caller (e.g. MCP tool).
     * Reuses the notification playback pipeline (NotifyPlayer -> AudioService).
     * `title` is displayed on the device screen while the audio plays.
     * Returns true if playback was successfully started, false otherwise.
     */
    bool StartNotification(std::string audio_url, std::vector<NotifySubtitle> subtitles,
                           const std::string& username = "", const std::string& password = "",
                           const std::string& title = "",
                           uint64_t start_byte_pos = 0, uint32_t start_pos_ms = 0,
                           uint32_t start_duration_ms = 0);

    // force=true 用于显式停止/切新歌；暂停期间内部流程调用时保留快照与状态
    void StopNotification(bool force = false);

    /**
     * Start music playback now; drops any queued follow-up tracks.
     * Returns true if playback was successfully started.
     */
    bool StartMusic(const MusicSource& source);

    // 播放整个曲目列表（网页播放器与 AI 共用同一会话）
    // Returns true if playback was successfully started.
    bool PlayMusicPlaylist(std::vector<MusicSource> tracks, int source_index, MusicPlayMode mode,
                           int start_index = 0, bool truncated = false);
    // Play a specific track in the current playlist
    // Returns true if playback was successfully started.
    bool PlayMusicTrackAt(int index);
    // Play next track in playlist
    // Returns true if playback was successfully started.
    bool PlayNextMusic();
    // Play previous track in playlist
    // Returns true if playback was successfully started.
    bool PlayPrevMusic();
    void StopMusicPlayback();
    bool PauseMusic();
    bool ResumeMusic();
    bool SeekMusic(uint32_t time_ms);
    void SetMusicMode(MusicPlayMode mode);

    // Internet radio source (only compiled when CONFIG_USE_RADIO is enabled)
    std::shared_ptr<radio::RadioSource> GetRadioSource() const { return radio_source_; }
    void SetRadioSource(std::shared_ptr<radio::RadioSource> source) { radio_source_ = std::move(source); }
    bool IsRadioPlaying() const { return radio_playing_; }
    void SetRadioPlaying(bool playing);
    void SetRadioStopHandler(std::function<void()> handler) { radio_stop_handler_ = std::move(handler); }
    void SetRadioStartHandler(std::function<void(const std::string&)> handler) {
        radio_start_handler_ = std::move(handler);
    }
    void StartRadio(const std::string& url);
    void StopRadio();
    void ArmRadioWatchdog(bool arm);
    // NVS 持久化的播放模式与选中音源（"music" 命名空间）
    static MusicPlayMode LoadPersistedMusicMode();
    static int LoadSelectedSource();
    static void SaveSelectedSource(int index);
    MusicStatus GetMusicStatus() const;

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void SendTtsFinished();
    bool IsNotifyPlayerBusy() const;
    NotifyPlayer& GetNotifyPlayer() { return notify_player_; }

    // PlaybackScheduler 访问（实现在 application.cc，避免头文件循环依赖）
    PlaybackScheduler& GetPlaybackScheduler();
    const PlaybackScheduler& GetPlaybackScheduler() const;

    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    esp_timer_handle_t startup_wake_word_hint_timer_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    std::string startup_wake_word_hint_message_;
    AudioService audio_service_;
    NotifyPlayer notify_player_;
    std::unique_ptr<PlaybackScheduler> playback_scheduler_;
    uint32_t notification_playback_id_ = 0;

    // 音乐播放会话：playlist_ 为音源全部曲目，order_ 为按模式生成的播放顺序
    mutable std::mutex music_mutex_;
    std::vector<MusicSource> music_playlist_;
    std::vector<size_t> music_order_;
    size_t music_cursor_ = 0;
    int music_source_index_ = -1;
    MusicPlayMode music_mode_ = MusicPlayMode::kSequential;
    bool music_active_ = false;
    bool music_paused_ = false;
    bool music_single_ = false;
    int music_fail_streak_ = 0;
    bool music_truncated_ = false;
    int music_position_ms_ = 0;

    // 单例暂停快照：一次性分配在 PSRAM，原地覆盖复用，不存字符串
    struct PlaybackSnapshot {
        uint32_t token = 0;
        int source_index = -1;
        int cursor = -1;
        uint32_t position_ms = 0;
        uint32_t duration_ms = 0;
        uint64_t byte_pos = 0;
        uint64_t file_size = 0;
        uint8_t mode = 0;
        bool valid = false;
    };
    PlaybackSnapshot* playback_snapshot_ = nullptr;

    // 网络电台：与音乐共用输出设备，切换时互斥
    std::shared_ptr<radio::RadioSource> radio_source_;
    std::function<void()> radio_stop_handler_;
    std::function<void(const std::string&)> radio_start_handler_;
    std::string pending_radio_url_;
    bool radio_playing_ = false;
    esp_timer_handle_t radio_watchdog_timer_ = nullptr;
    bool radio_watchdog_armed_ = false;
    static constexpr int kRadioBufferingTimeoutMs = 30000;
    static void RadioWatchdogCallback(void* arg);
    void HandleRadioWatchdogTimeout();
    void MaybeStartPendingRadio();

    void ClearMusicSessionLocked();
    // 单例快照：采集当前 IO 信息 / 更新目标位置 / 按快照重启（暂停与拖拽共用）
    bool CapturePlaybackSnapshot();
    void UpdateSnapshotPositionLocked(uint32_t time_ms);
    bool RestartFromSnapshot();
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;
    bool startup_completed_ = false;
    bool pending_startup_wake_word_hint_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    bool pending_listening_start_ = false;  // Waiting for playback to drain before starting listening (auto mode)
    bool is_tts_playing_ = false;  // 是否正在播放 TTS
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void BeginWakeWordInvoke(const std::string& wake_word);
    void ContinueWakeWordInvoke(const std::string& wake_word);
    void StartListeningAudio();
    void ConfigureWakeWordForListening();
    void ShowStartupWakeWordHint();
    void HideStartupWakeWordHint();
    void HandleNotificationFinished(uint32_t playback_id, bool success);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;

    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
