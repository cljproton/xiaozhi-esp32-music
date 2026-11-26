#include "audio_decoder_registry.h"

#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec_default.h>

AudioDecoderRegistry& AudioDecoderRegistry::GetInstance() {
    static AudioDecoderRegistry instance;
    return instance;
}

bool AudioDecoderRegistry::Acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0 && !registered_) {
        if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) {
            return false;
        }
        esp_audio_simple_dec_register_default();
        registered_ = true;
    }
    ++count_;
    return true;
}

void AudioDecoderRegistry::Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ > 0) --count_;
    // Keep the decoder table registered for the device lifetime: repeated
    // unregister/register churn overwrites entries and races other players.
}
