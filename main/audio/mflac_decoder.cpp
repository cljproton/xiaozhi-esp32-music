/*
 * micro-flac GMF decoder element implementation
 * Wraps micro_flac::FLACDecoder as a GMF audio element to avoid the
 * esp_audio_codec FLAC parser's 512KB search limit on large PICTURE blocks.
 */

#include "mflac_decoder.h"
#include "esp_log.h"
#include "esp_gmf_oal_mem.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_element.h"
#include "esp_gmf_audio_element.h"
#include "esp_gmf_port.h"

#include "micro_flac/flac_decoder.h"
#include "esp_fourcc.h"

#include <new>
#include <cstring>

static const char* TAG = "MF_DEC";

namespace mflac_decoder {

// Config stored as obj config (duplicated by pool across instances)
struct MflacConfig {
    uint32_t max_picture_size;
    uint32_t max_vorbis_comment_size;
    bool crc_check_enabled;
    std::atomic<uint32_t>* duration_ms_out;  // 精确时长输出指针（可为 null）
};

// Element state: base must be first field.
struct mflac_dec_t {
    esp_gmf_audio_element_t base;         // must be first
    micro_flac::FLACDecoder* decoder;     // heap allocated (placement new)
    uint8_t* output_buffer;               // heap (esp_gmf_oal_calloc)
    size_t output_buffer_size;
    bool header_ready;
    bool eos;
    uint32_t pts;
};

// Forward declarations of job callbacks (wired into ops in mflac_dec_new)
static esp_gmf_job_err_t mflac_dec_open(esp_gmf_element_handle_t handle, void* para);
static esp_gmf_job_err_t mflac_dec_process(esp_gmf_element_handle_t handle, void* para);
static esp_gmf_job_err_t mflac_dec_close(esp_gmf_element_handle_t handle, void* para);
static esp_gmf_job_err_t mflac_dec_reset(esp_gmf_element_handle_t handle, void* para);

static inline mflac_dec_t* get_dec(esp_gmf_element_handle_t handle) {
    return reinterpret_cast<mflac_dec_t*>(handle);
}

static inline MflacConfig* get_cfg(mflac_dec_t* dec) {
    return static_cast<MflacConfig*>(OBJ_GET_CFG(dec));
}

// ---------- lifecycle ----------

static esp_gmf_err_t mflac_dec_destroy(esp_gmf_obj_handle_t obj) {
    mflac_dec_t* dec = reinterpret_cast<mflac_dec_t*>(obj);
    if (!dec) return ESP_GMF_ERR_OK;
    if (dec->decoder) {
        dec->decoder->~FLACDecoder();
        esp_gmf_oal_free(dec->decoder);
        dec->decoder = nullptr;
    }
    if (dec->output_buffer) {
        esp_gmf_oal_free(dec->output_buffer);
        dec->output_buffer = nullptr;
    }
    MflacConfig* cfg = get_cfg(dec);
    if (cfg) {
        esp_gmf_oal_free(cfg);
        esp_gmf_obj_set_config((esp_gmf_obj_t*)dec, nullptr, 0);
    }
    esp_gmf_audio_el_deinit(dec);
    esp_gmf_oal_free(dec);
    return ESP_GMF_ERR_OK;
}

// new_obj callback: create a fresh instance from config (used by pool duplication)
static esp_gmf_err_t mflac_dec_new(void* cfg, esp_gmf_obj_handle_t* handle) {
    if (!handle) return ESP_GMF_ERR_INVALID_ARG;
    *handle = nullptr;
    MflacConfig* src_cfg = static_cast<MflacConfig*>(cfg);

    mflac_dec_t* dec = (mflac_dec_t*)esp_gmf_oal_calloc(1, sizeof(mflac_dec_t));
    if (!dec) return ESP_GMF_ERR_MEMORY_LACK;

    esp_gmf_obj_t* obj = (esp_gmf_obj_t*)dec;
    obj->new_obj = mflac_dec_new;
    obj->del_obj = mflac_dec_destroy;

    // Duplicate config
    MflacConfig cfg_copy = src_cfg ? *src_cfg : MflacConfig{50 * 1024, 4096, true, nullptr};
    MflacConfig* cfg_heap = (MflacConfig*)esp_gmf_oal_calloc(1, sizeof(MflacConfig));
    if (!cfg_heap) {
        esp_gmf_oal_free(dec);
        return ESP_GMF_ERR_MEMORY_LACK;
    }
    *cfg_heap = cfg_copy;
    esp_gmf_obj_set_config(obj, cfg_heap, sizeof(MflacConfig));

    // Allocate decoder via placement new
    void* dec_mem = esp_gmf_oal_calloc(1, sizeof(micro_flac::FLACDecoder));
    if (!dec_mem) {
        esp_gmf_oal_free(cfg_heap);
        esp_gmf_oal_free(dec);
        return ESP_GMF_ERR_MEMORY_LACK;
    }
    dec->decoder = new (dec_mem) micro_flac::FLACDecoder();

    // Configure decoder
    dec->decoder->set_max_metadata_size(micro_flac::FLAC_METADATA_TYPE_PICTURE, cfg_heap->max_picture_size);
    dec->decoder->set_max_metadata_size(micro_flac::FLAC_METADATA_TYPE_VORBIS_COMMENT, cfg_heap->max_vorbis_comment_size);
    dec->decoder->set_crc_check_enabled(cfg_heap->crc_check_enabled);

    dec->header_ready = false;
    dec->eos = false;
    dec->pts = 0;
    dec->output_buffer = nullptr;
    dec->output_buffer_size = 0;

    // Init element
    esp_gmf_element_cfg_t el_cfg = {};
    ESP_GMF_ELEMENT_IN_PORT_ATTR_SET(el_cfg.in_attr, ESP_GMF_EL_PORT_CAP_SINGLE, 0, 0,
        ESP_GMF_PORT_TYPE_BLOCK | ESP_GMF_PORT_TYPE_BYTE, 4096);
    // Output port must hold a full decoded FLAC frame (max_block_size * channels *
    // bytes_per_sample). 64KB covers up to 65535-block 16-bit stereo; also align to 16.
    ESP_GMF_ELEMENT_OUT_PORT_ATTR_SET(el_cfg.out_attr, ESP_GMF_EL_PORT_CAP_SINGLE, 0, 16,
        ESP_GMF_PORT_TYPE_BLOCK | ESP_GMF_PORT_TYPE_BYTE, 64 * 1024);
    el_cfg.dependency = false;
    esp_gmf_err_t ret = esp_gmf_audio_el_init(dec, &el_cfg);
    if (ret != ESP_GMF_ERR_OK) {
        dec->decoder->~FLACDecoder();
        esp_gmf_oal_free(dec->decoder);
        esp_gmf_oal_free(cfg_heap);
        esp_gmf_oal_free(dec);
        return ret;
    }

    // Wire up ops
    ESP_GMF_ELEMENT_GET(dec)->ops.open = mflac_dec_open;
    ESP_GMF_ELEMENT_GET(dec)->ops.process = mflac_dec_process;
    ESP_GMF_ELEMENT_GET(dec)->ops.close = mflac_dec_close;
    ESP_GMF_ELEMENT_GET(dec)->ops.reset = mflac_dec_reset;

    *handle = obj;
    return ESP_GMF_ERR_OK;
}

// ---------- job callbacks ----------

static esp_gmf_job_err_t mflac_dec_open(esp_gmf_element_handle_t handle, void* para) {
    (void)para;
    mflac_dec_t* dec = get_dec(handle);
    if (!dec || !dec->decoder) return ESP_GMF_JOB_ERR_FAIL;
    dec->decoder->reset();
    dec->header_ready = false;
    dec->eos = false;
    dec->pts = 0;
    ESP_LOGI(TAG, "mflac decoder opened (PICTURE max=%u)",
             get_cfg(dec)->max_picture_size);
    return ESP_GMF_JOB_ERR_OK;
}

static esp_gmf_job_err_t mflac_dec_process(esp_gmf_element_handle_t handle, void* para) {
    (void)para;
    mflac_dec_t* dec = get_dec(handle);
    if (!dec || !dec->decoder) return ESP_GMF_JOB_ERR_FAIL;

    esp_gmf_port_t* in_port = ESP_GMF_ELEMENT_GET(handle)->in;
    esp_gmf_port_t* out_port = ESP_GMF_ELEMENT_GET(handle)->out;

    esp_gmf_payload_t* in_load = nullptr;
    esp_gmf_payload_t* out_load = nullptr;

    esp_gmf_err_io_t in_ret = esp_gmf_port_acquire_in(in_port, &in_load,
        ESP_GMF_ELEMENT_GET(handle)->in_attr.data_size, in_port->wait_ticks);
    if (in_ret < 0) {
        return ESP_GMF_JOB_ERR_CONTINUE;
    }

    esp_gmf_err_io_t out_ret = esp_gmf_port_acquire_out(out_port, &out_load,
        ESP_GMF_ELEMENT_GET(handle)->out_attr.data_size, out_port->wait_ticks);
    if (out_ret < 0) {
        esp_gmf_port_release_in(in_port, in_load, in_port->wait_ticks);
        return ESP_GMF_JOB_ERR_CONTINUE;
    }

    if (dec->eos) {
        out_load->valid_size = 0;
        out_load->is_done = true;
        esp_gmf_port_release_out(out_port, out_load, out_port->wait_ticks);
        esp_gmf_port_release_in(in_port, in_load, in_port->wait_ticks);
        return ESP_GMF_JOB_ERR_DONE;
    }

    const uint8_t* input = in_load->buf;
    size_t input_len = in_load->valid_size;
    size_t bytes_consumed = 0;
    size_t samples_decoded = 0;
    bool in_done = in_load->is_done;

    // Ensure our internal output buffer is large enough (sized after header).
    auto result = dec->decoder->decode(input, input_len,
        dec->output_buffer, dec->output_buffer_size, bytes_consumed, samples_decoded);

    in_load->valid_size = bytes_consumed;
    esp_gmf_err_io_t in_rel = esp_gmf_port_release_in(in_port, in_load, in_port->wait_ticks);
    if (in_rel < 0) {
        esp_gmf_port_release_out(out_port, out_load, out_port->wait_ticks);
        return ESP_GMF_JOB_ERR_FAIL;
    }

    esp_gmf_job_err_t job_ret = ESP_GMF_JOB_ERR_OK;
    switch (result) {
        case micro_flac::FLAC_DECODER_HEADER_READY: {
            const auto& info = dec->decoder->get_stream_info();
            dec->output_buffer_size = info.max_block_size() * info.num_channels() * info.bytes_per_sample();
            if (dec->output_buffer) esp_gmf_oal_free(dec->output_buffer);
            dec->output_buffer = (uint8_t*)esp_gmf_oal_calloc(1, dec->output_buffer_size);
            if (!dec->output_buffer) {
                ESP_LOGE(TAG, "Failed to alloc output buffer %u", dec->output_buffer_size);
                out_load->valid_size = 0;
                out_load->is_done = true;
                esp_gmf_port_release_out(out_port, out_load, out_port->wait_ticks);
                return ESP_GMF_JOB_ERR_FAIL;
            }
            dec->header_ready = true;
            esp_gmf_info_sound_t snd = {};
            snd.format_id = ESP_FOURCC_FLAC;
            snd.sample_rates = info.sample_rate();
            snd.channels = info.num_channels();
            snd.bits = info.bits_per_sample();
            // 复用 bitrate 字段上报精确时长(ms)：FLAC STREAMINFO 有总采样数，
            // 可精确算出时长，避免上层用 CBR 估算（会被开头封面块带偏）。
            uint32_t duration_ms = 0;
            uint64_t total_samples = info.total_samples_per_channel();
            if (snd.sample_rates > 0 && total_samples > 0) {
                duration_ms = static_cast<uint32_t>(total_samples * 1000 / snd.sample_rates);
            }
            snd.bitrate = static_cast<int>(duration_ms);
            // 直写时长输出指针（GMF 只转发最后一个元素的 REPORT_INFO，本解码器
            // 在链首，其事件到不了用户回调，故用指针直传）
            MflacConfig* mcfg = get_cfg(dec);
            if (mcfg && mcfg->duration_ms_out && duration_ms > 0) {
                mcfg->duration_ms_out->store(duration_ms);
            }
            esp_gmf_audio_el_set_snd_info(handle, &snd);
            esp_gmf_element_notify_snd_info(handle, &snd);
            ESP_LOGI(TAG, "FLAC header: rate=%u ch=%u bits=%u maxblk=%u outbuf=%u dur=%u ms",
                     snd.sample_rates, snd.channels, snd.bits, info.max_block_size(),
                     dec->output_buffer_size, duration_ms);
            out_load->valid_size = 0;
            out_load->is_done = false;
            job_ret = ESP_GMF_JOB_ERR_CONTINUE;
            break;
        }
        case micro_flac::FLAC_DECODER_SUCCESS: {
            // samples_decoded is total interleaved samples (all channels already included)
            size_t out_bytes = samples_decoded * dec->decoder->get_stream_info().bytes_per_sample();
            // Copy decoded PCM from internal buffer into the GMF output payload,
            // growing awareness if the payload buffer is smaller.
            size_t copy_len = out_bytes;
            if (copy_len > out_load->buf_length) {
                ESP_LOGE(TAG, "Out buffer too small: need %u have %u", out_bytes, out_load->buf_length);
                out_load->valid_size = 0;
                out_load->is_done = true;
                esp_gmf_port_release_out(out_port, out_load, out_port->wait_ticks);
                return ESP_GMF_JOB_ERR_FAIL;
            }
            memcpy(out_load->buf, dec->output_buffer, copy_len);
            out_load->valid_size = copy_len;
            out_load->is_done = false;
            // PTS in ms, derived from decoded samples and sample rate
            out_load->pts = dec->pts;
            uint32_t channels = dec->decoder->get_stream_info().num_channels();
            uint32_t sample_rate = dec->decoder->get_stream_info().sample_rate();
            if (sample_rate > 0) {
                dec->pts += (uint32_t)((uint64_t)samples_decoded * 1000 / channels / sample_rate);
            }
            esp_gmf_audio_el_update_file_pos(handle, copy_len);
            job_ret = ESP_GMF_JOB_ERR_OK;
            break;
        }
        case micro_flac::FLAC_DECODER_END_OF_STREAM:
            dec->eos = true;
            out_load->valid_size = 0;
            out_load->is_done = true;
            job_ret = ESP_GMF_JOB_ERR_DONE;
            break;
        case micro_flac::FLAC_DECODER_NEED_MORE_DATA:
            out_load->valid_size = 0;
            out_load->is_done = in_done && (input_len == 0);
            if (out_load->is_done) dec->eos = true;
            job_ret = ESP_GMF_JOB_ERR_CONTINUE;
            break;
        default:
            ESP_LOGE(TAG, "micro-flac decode error: %d", (int)result);
            out_load->valid_size = 0;
            out_load->is_done = true;
            job_ret = ESP_GMF_JOB_ERR_FAIL;
            break;
    }

    esp_gmf_err_io_t out_rel = esp_gmf_port_release_out(out_port, out_load, out_port->wait_ticks);
    if (out_rel < 0) {
        return ESP_GMF_JOB_ERR_FAIL;
    }
    return job_ret;
}

static esp_gmf_job_err_t mflac_dec_close(esp_gmf_element_handle_t handle, void* para) {
    (void)para;
    mflac_dec_t* dec = get_dec(handle);
    if (dec && dec->decoder) {
        dec->decoder->reset();
    }
    if (dec) {
        dec->header_ready = false;
        dec->eos = false;
        dec->pts = 0;
    }
    return ESP_GMF_JOB_ERR_OK;
}

static esp_gmf_job_err_t mflac_dec_reset(esp_gmf_element_handle_t handle, void* para) {
    return mflac_dec_close(handle, para);
}

// ---------- public API ----------

esp_gmf_err_t mflac_decoder_init(esp_gmf_element_handle_t* handle,
                                 std::atomic<uint32_t>* duration_ms_out) {
    MflacConfig cfg = {50 * 1024, 4096, true, duration_ms_out};
    esp_gmf_err_t ret = mflac_dec_new(&cfg, (esp_gmf_obj_handle_t*)handle);
    if (ret == ESP_GMF_ERR_OK && *handle) {
        esp_gmf_obj_set_tag((esp_gmf_obj_t*)*handle, kMflacDecoderTag);
    }
    return ret;
}

} // namespace mflac_decoder