#include "audio_decoder_registry.h"

#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec_default.h>

AudioDecoderRegistry& AudioDecoderRegistry::GetInstance() {
    static AudioDecoderRegistry instance;
    return instance;
}

bool AudioDecoderRegistry::Acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) {
        if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) {
            return false;
        }
        esp_audio_simple_dec_register_default();
    }
    ++count_;
    return true;
}

void AudioDecoderRegistry::Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ <= 0) return;
    if (--count_ == 0) {
        esp_audio_dec_unregister_default();
        esp_audio_simple_dec_unregister_default();
    }
}
