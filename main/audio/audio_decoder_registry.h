#pragma once

#include <mutex>

// Shared ownership of the process-wide esp_audio codec registry.
// Music/radio (GMF) and ebook (low-level simple decoder) must not register or
// unregister decoders independently: one user's cleanup would break the other.
class AudioDecoderRegistry {
public:
    static AudioDecoderRegistry& GetInstance();

    bool Acquire();
    void Release();

    AudioDecoderRegistry(const AudioDecoderRegistry&) = delete;
    AudioDecoderRegistry& operator=(const AudioDecoderRegistry&) = delete;

private:
    AudioDecoderRegistry() = default;
    std::mutex mutex_;
    int count_ = 0;
    bool registered_ = false;
};
