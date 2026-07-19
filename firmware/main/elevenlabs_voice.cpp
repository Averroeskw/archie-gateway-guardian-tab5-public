#include "elevenlabs_voice.hpp"

#include "bsp/m5stack_tab5.h"
#include "cJSON.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

constexpr size_t kMaxPcmBytes = 3 * 1024 * 1024;
constexpr uint32_t kSampleRate = 24000;

struct Download {
    uint8_t *data = nullptr;
    size_t size = 0;
    size_t capacity = 0;
    bool overflow = false;
};

esp_err_t http_event(esp_http_client_event_t *event)
{
    auto *download = static_cast<Download *>(event->user_data);
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data == nullptr ||
        event->data_len <= 0) {
        return ESP_OK;
    }
    size_t incoming = static_cast<size_t>(event->data_len);
    if (download->size + incoming > download->capacity) {
        download->overflow = true;
        return ESP_FAIL;
    }
    std::memcpy(download->data + download->size, event->data, incoming);
    download->size += incoming;
    return ESP_OK;
}

}  // namespace

esp_err_t ElevenLabsVoice::begin(const ElevenLabsVoiceConfig &config, LogCallback log,
                                 void *ctx)
{
    if (config.api_key.empty() || config.voice_id.empty()) {
        return ESP_ERR_INVALID_ARG;
    }
    config_ = config;
    log_ = log;
    log_ctx_ = ctx;
    esp_codec_dev_handle_t speaker = bsp_audio_codec_speaker_init();
    if (speaker == nullptr) {
        return ESP_FAIL;
    }
    speaker_ = speaker;
    set_volume(config.volume);
    ready_ = true;
    return ESP_OK;
}

void ElevenLabsVoice::set_volume(int percent)
{
    config_.volume = std::clamp(percent, 0, 100);
    if (speaker_ != nullptr) {
        esp_codec_dev_set_out_vol(static_cast<esp_codec_dev_handle_t>(speaker_),
                                  config_.volume);
    }
}

esp_err_t ElevenLabsVoice::speak(const char *text)
{
    if (!ready_ || text == nullptr || text[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (busy_.exchange(true)) {
        return ESP_ERR_INVALID_STATE;
    }
    pending_text_ = text;
    if (xTaskCreate(speak_task, "eleven_voice", 12288, this, 4, nullptr) != pdPASS) {
        busy_ = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void ElevenLabsVoice::speak_task(void *arg)
{
    auto *self = static_cast<ElevenLabsVoice *>(arg);
    self->synthesize_and_play();
    self->pending_text_.clear();
    self->busy_ = false;
    vTaskDelete(nullptr);
}

void ElevenLabsVoice::emit(const char *line, bool error)
{
    if (log_ != nullptr) {
        log_(line, error, log_ctx_);
    }
}

void ElevenLabsVoice::synthesize_and_play()
{
    auto *pcm = static_cast<uint8_t *>(
        heap_caps_malloc(kMaxPcmBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (pcm == nullptr) {
        emit("voice: no PSRAM for audio", true);
        return;
    }
    Download download{pcm, 0, kMaxPcmBytes, false};

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "text", pending_text_.c_str());
    cJSON_AddStringToObject(root, "model_id", "eleven_flash_v2_5");
    std::unique_ptr<char, decltype(&cJSON_free)> body(cJSON_PrintUnformatted(root), cJSON_free);
    cJSON_Delete(root);
    if (!body) {
        heap_caps_free(pcm);
        emit("voice: request allocation failed", true);
        return;
    }

    std::string url = "https://api.elevenlabs.io/v1/text-to-speech/" +
                      config_.voice_id + "?output_format=pcm_24000";
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.event_handler = http_event;
    cfg.user_data = &download;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 90000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        heap_caps_free(pcm);
        emit("voice: HTTPS client unavailable", true);
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "audio/pcm");
    esp_http_client_set_header(client, "xi-api-key", config_.api_key.c_str());
    esp_http_client_set_post_field(client, body.get(), static_cast<int>(std::strlen(body.get())));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300 || download.overflow ||
        download.size < 2) {
        heap_caps_free(pcm);
        char line[72];
        std::snprintf(line, sizeof(line), "voice: synthesis failed (HTTPS %d)", status);
        emit(line, true);
        return;
    }

    esp_codec_dev_sample_info_t sample = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = kSampleRate,
        .mclk_multiple = 0,
    };
    auto speaker = static_cast<esp_codec_dev_handle_t>(speaker_);
    err = esp_codec_dev_open(speaker, &sample);
    if (err == ESP_OK) {
        err = esp_codec_dev_write(speaker, pcm, download.size);
        static uint8_t silence[1920] = {};
        for (int i = 0; err == ESP_OK && i < 4; ++i) {
            esp_codec_dev_write(speaker, silence, sizeof(silence));
        }
        esp_codec_dev_close(speaker);
    }
    heap_caps_free(pcm);
    emit(err == ESP_OK ? "voice: transmission complete" : "voice: playback failed",
         err != ESP_OK);
}
