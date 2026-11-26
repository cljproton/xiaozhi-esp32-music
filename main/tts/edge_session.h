#pragma once

#include "audio_ring.h"
#include "tts_client.h"

#include <esp_websocket_client.h>
#include <atomic>
#include <string>

// Reusable Edge TTS WebSocket session. Connects once, sends speech.config
// once, and accepts multiple SSML requests (each with its own X-RequestId)
// over the same connection. Binary audio frames are stripped of their header
// and pushed into a PSRAM ring buffer for the decode task.
class EdgeSession {
public:
    static EdgeSession& GetInstance();

    bool Connect(const std::string& voice, int speed);
    void Disconnect();
    bool IsConnected() const { return connected_; }

    // Queue a sentence for synthesis (non-blocking). Returns false if not connected.
    bool Speak(const std::string& text);
    // Read synthesized MP3 bytes (from ring buffer). Returns bytes read.
    size_t ReadAudio(uint8_t* out, size_t max) { return ring_.Read(out, max); }
    size_t AudioAvailable() const { return ring_.Available(); }
    // True when all queued sentences have finished (turn.end received).
    bool Idle() const { return pending_ == 0; }
    int PendingCount() const { return pending_.load(); }
    // Number of turn.end markers received (sentences fully synthesized).
    int DoneCount() const { return done_count_.load(); }
    void Reset();  // clear ring + pending counters
    void ResetCounters();  // keep buffered audio, forget owed requests

private:
    EdgeSession() : ring_(512 * 1024) {}
    static void EventHandler(void* arg, esp_event_base_t, int32_t event, void* data);
    void WriteAudio(const uint8_t* data, size_t len);

    esp_websocket_client_handle_t client_ = nullptr;
    AudioRing ring_;
    std::atomic<bool> connected_{false};
    std::atomic<int> pending_{0};
    std::atomic<int> done_count_{0};
    bool id3_sent_ = false;
    std::string voice_;
    int speed_ = 0;
};
