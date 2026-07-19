#pragma once

#include "esp_err.h"
#include "esp_http_client.h"

#include <atomic>
#include <cstdint>
#include <string>

struct DirectAiConfig {
    std::string provider;  // openai, claude, or custom (OpenAI-compatible)
    std::string base_url;
    std::string api_key;
    std::string model;
};

struct DirectAiCallbacks {
    void (*on_chat_text)(const char *persona, const char *text, bool done, void *ctx);
    void (*on_status)(const char *persona, const char *state, uint32_t tokens_in,
                      uint32_t tokens_out, void *ctx);
    void (*on_link)(bool connected, void *ctx);
    void (*on_log_line)(const char *line, void *ctx);
    void *ctx;
};

// Lightweight HTTPS client for users who want a provider key on the device
// instead of running a gateway. It intentionally implements text chat only;
// agent tools, approvals and persistent memory remain gateway features.
class DirectAiClient {
public:
    esp_err_t begin(const DirectAiConfig &config, const DirectAiCallbacks &callbacks);
    void stop();
    esp_err_t send_chat(const char *persona, const char *text);
    esp_err_t send_stop();
    esp_err_t send_clear(const char *persona);

    bool connected() const { return ready_.load(std::memory_order_acquire); }
    uint32_t rx_bytes() const { return rx_bytes_.load(std::memory_order_relaxed); }
    uint32_t tx_bytes() const { return tx_bytes_.load(std::memory_order_relaxed); }

private:
    static void request_task(void *arg);
    static esp_err_t http_event(esp_http_client_event_t *event);
    void perform_request();
    void emit_log(const char *line);

    DirectAiConfig config_;
    DirectAiCallbacks callbacks_ = {};
    std::string pending_persona_;
    std::string pending_text_;
    std::string response_body_;
    std::atomic<bool> ready_{false};
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<uint32_t> rx_bytes_{0};
    std::atomic<uint32_t> tx_bytes_{0};
};
