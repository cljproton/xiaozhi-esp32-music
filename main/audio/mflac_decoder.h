/*
 * micro-flac GMF decoder element
 * Wraps micro_flac::FLACDecoder as a GMF audio element.
 * Bypasses the 512KB parser limit of esp_audio_codec for FLAC files with large PICTURE blocks.
 */

#pragma once

#include "esp_gmf_audio_dec.h"
#include "esp_gmf_element.h"
#include "esp_gmf_err.h"
#include "esp_gmf_info.h"
#include "esp_gmf_audio_element.h"

#include <cstdint>

namespace mflac_decoder {

// Create and initialize the mflac decoder element (tag: "aud_dec_mflac").
// Registers into a GMF pool; downstream elements identical to the standard aud_dec.
esp_gmf_err_t mflac_decoder_init(esp_gmf_element_handle_t* handle);

// Element tag used for pipeline registration / lookup.
constexpr const char* kMflacDecoderTag = "aud_dec_mflac";

} // namespace mflac_decoder