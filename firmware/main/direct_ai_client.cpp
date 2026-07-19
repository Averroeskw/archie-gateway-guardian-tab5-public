#include "direct_ai_client.hpp"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

struct JsonDoc {
    cJSON *node;
    explicit JsonDoc(cJSON *value) : node(value) {}
    ~JsonDoc() { cJSON_Delete(node); }
};

const char *json_string(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

uint32_t json_u32(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) && item->valuedouble >= 0
               ? static_cast<uint32_t>(item->valuedouble)
               : 0;
}

std::string trim_slash(std::string value)
{
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

}  // namespace

esp_err_t DirectAiClient::begin(const DirectAiConfig &config,
                                const DirectAiCallbacks &callbacks)
{
    const bool key_required = config.provider != "custom";
    if (config.base_url.empty() || config.model.empty() ||
        (key_required && config.api_key.empty())) {
        return ESP_ERR_INVALID_ARG;
    }
    config_ = config;
    callbacks_ = callbacks;
    ready_ = true;
    if (callbacks_.on_link != nullptr) {
        callbacks_.on_link(true, callbacks_.ctx);
    }
    return ESP_OK;
}

void DirectAiClient::stop()
{
    cancel_ = true;
    bool was_ready = ready_.exchange(false);
    if (was_ready && callbacks_.on_link != nullptr) {
        callbacks_.on_link(false, callbacks_.ctx);
    }
}

esp_err_t DirectAiClient::send_chat(const char *persona, const char *text)
{
    if (!ready_ || text == nullptr || text[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (busy_.exchange(true)) {
        return ESP_ERR_INVALID_STATE;
    }
    pending_persona_ = persona != nullptr ? persona : "archie";
    pending_text_ = text;
    cancel_ = false;
    if (xTaskCreate(request_task, "direct_ai", 12288, this, 4, nullptr) != pdPASS) {
        busy_ = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t DirectAiClient::send_stop()
{
    cancel_ = true;
    return ESP_OK;
}

esp_err_t DirectAiClient::send_clear(const char *)
{
    // Requests are stateless in direct mode; there is no server-side session.
    return ESP_OK;
}

void DirectAiClient::request_task(void *arg)
{
    static_cast<DirectAiClient *>(arg)->perform_request();
    static_cast<DirectAiClient *>(arg)->busy_ = false;
    vTaskDelete(nullptr);
}

esp_err_t DirectAiClient::http_event(esp_http_client_event_t *event)
{
    auto *self = static_cast<DirectAiClient *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data != nullptr && event->data_len > 0) {
        self->response_body_.append(static_cast<const char *>(event->data), event->data_len);
        self->rx_bytes_.fetch_add(static_cast<uint32_t>(event->data_len), std::memory_order_relaxed);
    }
    return ESP_OK;
}

void DirectAiClient::emit_log(const char *line)
{
    if (callbacks_.on_log_line != nullptr) {
        callbacks_.on_log_line(line, callbacks_.ctx);
    }
}

void DirectAiClient::perform_request()
{
    const bool claude = config_.provider == "claude";
    std::string url = trim_slash(config_.base_url) + (claude ? "/v1/messages" : "/chat/completions");

    JsonDoc request(cJSON_CreateObject());
    cJSON_AddStringToObject(request.node, "model", config_.model.c_str());
    if (config_.provider == "openclaw") {
        // Stable conversation key for the official OpenClaw Chat Completions
        // endpoint. No owner name, device serial or other personal data.
        cJSON_AddStringToObject(request.node, "user", "archie-tab5");
    }
    if (claude) {
        cJSON_AddNumberToObject(request.node, "max_tokens", 1024);
        cJSON_AddStringToObject(
            request.node, "system",
            "You are Archie, a concise cosmic gateway guardian. Be useful and direct.");
    }
    cJSON *messages = cJSON_AddArrayToObject(request.node, "messages");
    if (!claude) {
        cJSON *system = cJSON_CreateObject();
        cJSON_AddStringToObject(system, "role", "system");
        cJSON_AddStringToObject(
            system, "content",
            "You are Archie, a concise cosmic gateway guardian. Be useful and direct.");
        cJSON_AddItemToArray(messages, system);
    }
    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", pending_text_.c_str());
    cJSON_AddItemToArray(messages, user);

    std::unique_ptr<char, decltype(&cJSON_free)> body(cJSON_PrintUnformatted(request.node),
                                                      cJSON_free);
    if (!body) {
        emit_log("direct AI: request allocation failed");
        return;
    }
    tx_bytes_.fetch_add(static_cast<uint32_t>(std::strlen(body.get())), std::memory_order_relaxed);
    if (callbacks_.on_status != nullptr) {
        callbacks_.on_status(pending_persona_.c_str(), "thinking", 0, 0, callbacks_.ctx);
    }

    response_body_.clear();
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.event_handler = http_event;
    cfg.user_data = this;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 60000;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        emit_log("direct AI: HTTPS client unavailable");
        return;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (claude) {
        esp_http_client_set_header(client, "x-api-key", config_.api_key.c_str());
        esp_http_client_set_header(client, "anthropic-version", "2023-06-01");
    } else if (!config_.api_key.empty()) {
        std::string auth = "Bearer " + config_.api_key;
        esp_http_client_set_header(client, "Authorization", auth.c_str());
    }
    esp_http_client_set_post_field(client, body.get(), static_cast<int>(std::strlen(body.get())));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (cancel_) {
        emit_log("direct AI: request cancelled");
        if (callbacks_.on_status != nullptr) {
            callbacks_.on_status(pending_persona_.c_str(), "idle", 0, 0, callbacks_.ctx);
        }
        return;
    }
    if (err != ESP_OK || status < 200 || status >= 300) {
        char line[80];
        std::snprintf(line, sizeof(line), "direct AI: HTTPS %d (%s)", status,
                      esp_err_to_name(err));
        emit_log(line);
        if (callbacks_.on_status != nullptr) {
            callbacks_.on_status(pending_persona_.c_str(), "error", 0, 0, callbacks_.ctx);
        }
        return;
    }

    JsonDoc response(cJSON_ParseWithLength(response_body_.data(), response_body_.size()));
    const char *text = "";
    uint32_t tokens_in = 0, tokens_out = 0;
    if (response.node != nullptr && claude) {
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(response.node, "content");
        const cJSON *first = cJSON_GetArrayItem(content, 0);
        text = first != nullptr ? json_string(first, "text") : "";
        const cJSON *usage = cJSON_GetObjectItemCaseSensitive(response.node, "usage");
        tokens_in = usage != nullptr ? json_u32(usage, "input_tokens") : 0;
        tokens_out = usage != nullptr ? json_u32(usage, "output_tokens") : 0;
    } else if (response.node != nullptr) {
        const cJSON *choices = cJSON_GetObjectItemCaseSensitive(response.node, "choices");
        const cJSON *first = cJSON_GetArrayItem(choices, 0);
        const cJSON *message = first != nullptr
                                   ? cJSON_GetObjectItemCaseSensitive(first, "message")
                                   : nullptr;
        text = message != nullptr ? json_string(message, "content") : "";
        const cJSON *usage = cJSON_GetObjectItemCaseSensitive(response.node, "usage");
        tokens_in = usage != nullptr ? json_u32(usage, "prompt_tokens") : 0;
        tokens_out = usage != nullptr ? json_u32(usage, "completion_tokens") : 0;
    }

    if (text[0] == '\0') {
        emit_log("direct AI: provider returned no text");
        if (callbacks_.on_status != nullptr) {
            callbacks_.on_status(pending_persona_.c_str(), "error", tokens_in, tokens_out,
                                 callbacks_.ctx);
        }
        return;
    }
    if (callbacks_.on_chat_text != nullptr) {
        callbacks_.on_chat_text(pending_persona_.c_str(), text, true, callbacks_.ctx);
    }
    if (callbacks_.on_status != nullptr) {
        callbacks_.on_status(pending_persona_.c_str(), "idle", tokens_in, tokens_out,
                             callbacks_.ctx);
    }
}
