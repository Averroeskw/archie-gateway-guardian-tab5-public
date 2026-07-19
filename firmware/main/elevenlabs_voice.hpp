#pragma once

#include "esp_err.h"

#include <atomic>
#include <string>

struct ElevenLabsVoiceConfig {
    std::string api_key;
    std::string voice_id;
    int volume = 60;
};

// Optional reply voice for every transport. ElevenLabs can return raw
// 24 kHz mono PCM, so the Tab5 needs no MP3 decoder and can stream the bytes
// straight through its ES8388 speaker codec.
class ElevenLabsVoice {
public:
    using LogCallback = void (*)(const char *line, bool error, void *ctx);

    esp_err_t begin(const ElevenLabsVoiceConfig &config, LogCallback log, void *ctx);
    esp_err_t speak(const char *text);
    void set_volume(int percent);
    bool ready() const { return ready_.load(std::memory_order_acquire); }
    bool busy() const { return busy_.load(std::memory_order_acquire); }

private:
    static void speak_task(void *arg);
    void synthesize_and_play();
    void emit(const char *line, bool error);

    ElevenLabsVoiceConfig config_;
    LogCallback log_ = nullptr;
    void *log_ctx_ = nullptr;
    void *speaker_ = nullptr;
    std::string pending_text_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> busy_{false};
};
