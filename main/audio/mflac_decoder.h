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
#include <atomic>

namespace mflac_decoder {

// Create and initialize the mflac decoder element (tag: "aud_dec_mflac").
// `duration_ms_out`: 可选，解码器解析出 STREAMINFO 后写入精确时长(ms)。
// 因 GMF 只把最后一个元素的 REPORT_INFO 转发给用户回调，解码器时长改由此指针直传。
esp_gmf_err_t mflac_decoder_init(esp_gmf_element_handle_t* handle,
                                 std::atomic<uint32_t>* duration_ms_out = nullptr);

// Element tag used for pipeline registration / lookup.
constexpr const char* kMflacDecoderTag = "aud_dec_mflac";

} // namespace mflac_decoder