#pragma once

#include <functional>
#include <string>

// Streaming TTS result: PCM/mp3 bytes delivered as they arrive.
using TtsAudioCallback = std::function<void(const uint8_t* data, size_t len)>;
using TtsDoneCallback = std::function<void(bool ok, const std::string& error)>;

struct TtsRequest {
    std::string text;
    std::string voice = "zh-CN-XiaoxiaoNeural";
    int speed = 0;  // -50..50 percentage
    TtsAudioCallback on_audio;
    TtsDoneCallback on_done;
};

// Synthesize text into a stream of mp3 frames. The call is synchronous and
// blocks until synthesis finishes or fails; audio is delivered via callbacks.
bool TtsSynthesize(const TtsRequest& request);
// Sec-MS-GEC v2 token for the current time (shared with EdgeSession).
std::string TtsMakeSecMsGec();

// Current provider from NVS ("edge" default, or "http").
std::string TtsGetProvider();
void TtsSetProvider(const std::string& provider);
std::string TtsGetVoice();
void TtsSetVoice(const std::string& voice);
int TtsGetSpeed();
void TtsSetSpeed(int speed);
std::string TtsGetHttpUrl();
void TtsSetHttpUrl(const std::string& url);
std::string TtsGetApiKey();
void TtsSetApiKey(const std::string& key);

// Freeze-dried list of common Edge TTS zh-CN voices for the Web dropdown.
const char* const* TtsEdgeVoices(size_t* count);
