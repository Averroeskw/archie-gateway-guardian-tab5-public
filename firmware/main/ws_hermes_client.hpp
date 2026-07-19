#pragma once

#include "esp_err.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// WebSocket bridge to a Hermes Gateway (see gateway/ for reference
// implementations). Speaks the JSON envelope protocol in docs/GATEWAY_API.md.
//
// Threading: esp_websocket_client delivers events on its own task. All
// callbacks below fire on that task — implementations must marshal into the
// LVGL task themselves (bsp_display_lock) before touching widgets.

// One entry from a `tasks` envelope (the console's TASKS panel).
struct HermesTaskItem {
    char label[48];
    bool done;
};
constexpr int kMaxTaskItems = 6;

struct WsHermesCallbacks {
    // Streamed assistant text. done=true marks the end of one reply.
    void (*on_chat_text)(const char *persona, const char *text, bool done, void *ctx);
    // Gateway status envelope: agent state + token counters.
    void (*on_status)(const char *persona, const char *state, uint32_t tokens_in, uint32_t tokens_out, void *ctx);
    // Result of a camera_frame inference round-trip.
    void (*on_vision_result)(const char *summary, float confidence, void *ctx);
    // TTS audio for the last reply is ready to fetch (HTTP URL, 16k mono WAV).
    void (*on_tts_ready)(const char *url, void *ctx);
    // Link state: connected/disconnected (also mirrored into TelemetryModel).
    void (*on_link)(bool connected, void *ctx);
    // Human-readable gateway/system line for the session log.
    void (*on_log_line)(const char *line, void *ctx);
    // Task list snapshot for the TASKS panel (up to kMaxTaskItems).
    void (*on_tasks)(const HermesTaskItem *items, int count, void *ctx);
    void *ctx;
};

struct WsHermesConfig {
    std::string uri;    // ws://host:8787/ws/tab5
    std::string token;  // shared secret; sent in the hello envelope
    std::string backend;  // hermes or openclaw; lets one adapter route both
};

class WsHermesClient {
public:
    esp_err_t begin(const WsHermesConfig &config, const WsHermesCallbacks &callbacks);
    void stop();
    bool connected() const { return connected_.load(std::memory_order_acquire); }

    // All senders are safe from any task; they serialize JSON and enqueue on
    // the websocket client's own send path.
    esp_err_t send_chat(const char *persona, const char *text);
    // wake=true marks a hands-free sentry clip: the gateway only acts when
    // the transcript begins with the wake word.
    esp_err_t send_ptt_audio(const uint8_t *pcm16, size_t bytes, uint32_t sample_rate, const char *persona,
                             bool wake = false);
    // mode: "snapshot" (user-initiated INFER) or "sentry" (motion event —
    // the gateway may forward an alert to its owner channel).
    esp_err_t send_camera_frame(const uint8_t *jpeg, size_t bytes, const char *prompt,
                                const char *mode = "snapshot");
    esp_err_t send_stop();
    // Ask the gateway to drop the persona's conversation memory (panel CLEAR).
    esp_err_t send_clear(const char *persona);

    uint32_t rx_bytes() const { return rx_bytes_.load(std::memory_order_relaxed); }
    uint32_t tx_bytes() const { return tx_bytes_.load(std::memory_order_relaxed); }

private:
    static void event_handler(void *arg, const char *base, int32_t event_id, void *event_data);
    void handle_text_frame(const char *data, size_t len);
    esp_err_t send_json(const char *json, size_t len);
    esp_err_t send_hello();

    void *client_ = nullptr;  // esp_websocket_client_handle_t
    WsHermesCallbacks callbacks_ = {};
    WsHermesConfig config_;
    std::atomic<bool> connected_{false};
    std::atomic<uint32_t> rx_bytes_{0};
    std::atomic<uint32_t> tx_bytes_{0};
    // Reassembly buffer: esp_websocket_client can deliver one WS message in
    // several DATA events; envelopes (esp. camera/ptt acks) can span them.
    std::string rx_assembly_;
};
