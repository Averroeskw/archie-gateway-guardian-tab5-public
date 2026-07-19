#include "ws_hermes_client.hpp"

#include "app_config.hpp"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/base64.h"

#include <cstring>
#include <memory>
#include <vector>

static const char *TAG = "ws_hermes";

// Reconnect handled by esp_websocket_client itself (auto-reconnect with this
// backoff). Keepalive pings keep NAT/tailnet paths from idling out.
static constexpr int kReconnectTimeoutMs = 5000;
static constexpr int kKeepalivePingSec = 15;
// PTT clips and camera frames go up base64-encoded inside one text frame.
// 10s of 16 kHz mono PCM16 is 320 KB -> ~427 KB base64: fits PSRAM easily,
// stays far under the gateway's 8 MB frame cap.
static constexpr size_t kMaxBinaryPayload = 1024 * 1024;

namespace {

std::string base64_encode(const uint8_t *data, size_t len)
{
    size_t needed = 0;
    mbedtls_base64_encode(nullptr, 0, &needed, data, len);
    std::string out;
    out.resize(needed);
    size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(out.data()), out.size(), &written, data, len) != 0) {
        return {};
    }
    out.resize(written);
    return out;
}

// RAII for cJSON roots so early returns can't leak.
struct JsonDoc {
    cJSON *node;
    explicit JsonDoc(cJSON *n) : node(n) {}
    ~JsonDoc() { cJSON_Delete(node); }
    JsonDoc(const JsonDoc &) = delete;
    JsonDoc &operator=(const JsonDoc &) = delete;
};

const char *json_str(const cJSON *obj, const char *key, const char *fallback = "")
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : fallback;
}

uint32_t json_u32(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) && item->valuedouble >= 0 ? static_cast<uint32_t>(item->valuedouble) : 0;
}

}  // namespace

esp_err_t WsHermesClient::begin(const WsHermesConfig &config, const WsHermesCallbacks &callbacks)
{
    config_ = config;
    callbacks_ = callbacks;
    if (config_.uri.empty()) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = config_.uri.c_str();
    if (config_.uri.rfind("wss://", 0) == 0) {
        // TLS (e.g. a Tailscale Funnel hostname): trust the public CA bundle.
        ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    ws_cfg.reconnect_timeout_ms = kReconnectTimeoutMs;
    ws_cfg.network_timeout_ms = 20000;
    ws_cfg.ping_interval_sec = kKeepalivePingSec;
    // A missed pong should NOT tear down a working link — through a TLS Funnel
    // proxy the round-trips are unreliable and this dropped the link ~47s in;
    // on a direct LAN ws:// it is simply harmless (the gateway's own 30s
    // heartbeat keeps the connection honest either way). Applied to all URIs.
    ws_cfg.disable_pingpong_discon = true;
    ws_cfg.buffer_size = 4096;
    ws_cfg.task_stack = 8192;

    esp_websocket_client_handle_t client = esp_websocket_client_init(&ws_cfg);
    if (client == nullptr) {
        return ESP_FAIL;
    }
    client_ = client;
    ESP_RETURN_ON_ERROR(
        esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, event_handler, this), TAG,
        "register ws events");
    return esp_websocket_client_start(client);
}

void WsHermesClient::stop()
{
    if (client_ != nullptr) {
        esp_websocket_client_stop(static_cast<esp_websocket_client_handle_t>(client_));
        esp_websocket_client_destroy(static_cast<esp_websocket_client_handle_t>(client_));
        client_ = nullptr;
    }
    connected_ = false;
}

esp_err_t WsHermesClient::send_json(const char *json, size_t len)
{
    if (client_ == nullptr || !connected_) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_text(static_cast<esp_websocket_client_handle_t>(client_), json,
                                              static_cast<int>(len), pdMS_TO_TICKS(10000));
    if (sent < 0) {
        ESP_LOGW(TAG, "ws send failed (%d bytes)", static_cast<int>(len));
        return ESP_FAIL;
    }
    tx_bytes_.fetch_add(static_cast<uint32_t>(sent), std::memory_order_relaxed);
    return ESP_OK;
}

esp_err_t WsHermesClient::send_hello()
{
    JsonDoc doc(cJSON_CreateObject());
    cJSON_AddStringToObject(doc.node, "type", "hello");
    cJSON_AddStringToObject(doc.node, "client", "archie-gateway-guardian-tab5");
    cJSON_AddStringToObject(doc.node, "version", ARCHIE_OS_VERSION);
    cJSON_AddStringToObject(doc.node, "backend",
                            config_.backend.empty() ? "hermes" : config_.backend.c_str());
    cJSON_AddStringToObject(doc.node, "token", config_.token.c_str());
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(doc.node), cJSON_free);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    return send_json(json.get(), std::strlen(json.get()));
}

esp_err_t WsHermesClient::send_chat(const char *persona, const char *text)
{
    JsonDoc doc(cJSON_CreateObject());
    cJSON_AddStringToObject(doc.node, "type", "chat");
    cJSON_AddStringToObject(doc.node, "persona", persona);
    cJSON_AddStringToObject(doc.node, "text", text);
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(doc.node), cJSON_free);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    return send_json(json.get(), std::strlen(json.get()));
}

esp_err_t WsHermesClient::send_ptt_audio(const uint8_t *pcm16, size_t bytes, uint32_t sample_rate,
                                         const char *persona, bool wake)
{
    if (bytes == 0 || bytes > kMaxBinaryPayload) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::string encoded = base64_encode(pcm16, bytes);
    if (encoded.empty()) {
        return ESP_ERR_NO_MEM;
    }
    JsonDoc doc(cJSON_CreateObject());
    cJSON_AddStringToObject(doc.node, "type", "ptt_audio");
    cJSON_AddStringToObject(doc.node, "format", "pcm16");
    cJSON_AddNumberToObject(doc.node, "sample_rate", sample_rate);
    cJSON_AddStringToObject(doc.node, "persona", persona);
    cJSON_AddBoolToObject(doc.node, "wake", wake);
    cJSON_AddStringToObject(doc.node, "audio_base64", encoded.c_str());
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(doc.node), cJSON_free);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    return send_json(json.get(), std::strlen(json.get()));
}

esp_err_t WsHermesClient::send_camera_frame(const uint8_t *jpeg, size_t bytes, const char *prompt,
                                            const char *mode)
{
    if (bytes == 0 || bytes > kMaxBinaryPayload) {
        return ESP_ERR_INVALID_SIZE;
    }
    std::string encoded = base64_encode(jpeg, bytes);
    if (encoded.empty()) {
        return ESP_ERR_NO_MEM;
    }
    JsonDoc doc(cJSON_CreateObject());
    cJSON_AddStringToObject(doc.node, "type", "camera_frame");
    cJSON_AddStringToObject(doc.node, "source", "tab5_local_cam");
    cJSON_AddStringToObject(doc.node, "mode", mode);
    cJSON_AddStringToObject(doc.node, "prompt", prompt);
    cJSON_AddStringToObject(doc.node, "jpeg_base64", encoded.c_str());
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(doc.node), cJSON_free);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    return send_json(json.get(), std::strlen(json.get()));
}

esp_err_t WsHermesClient::send_stop()
{
    static const char kStop[] = "{\"type\":\"stop\"}";
    return send_json(kStop, sizeof(kStop) - 1);
}

esp_err_t WsHermesClient::send_clear(const char *persona)
{
    JsonDoc doc(cJSON_CreateObject());
    cJSON_AddStringToObject(doc.node, "type", "clear");
    cJSON_AddStringToObject(doc.node, "persona", persona);
    std::unique_ptr<char, decltype(&cJSON_free)> json(cJSON_PrintUnformatted(doc.node), cJSON_free);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    return send_json(json.get(), std::strlen(json.get()));
}

void WsHermesClient::handle_text_frame(const char *data, size_t len)
{
    JsonDoc doc(cJSON_ParseWithLength(data, len));
    if (doc.node == nullptr) {
        ESP_LOGW(TAG, "unparseable envelope (%u bytes)", static_cast<unsigned>(len));
        return;
    }
    const char *type = json_str(doc.node, "type");
    if (std::strcmp(type, "chat_delta") == 0 || std::strcmp(type, "chat_done") == 0) {
        if (callbacks_.on_chat_text != nullptr) {
            callbacks_.on_chat_text(json_str(doc.node, "persona", "hermes"), json_str(doc.node, "text"),
                                    std::strcmp(type, "chat_done") == 0, callbacks_.ctx);
        }
    } else if (std::strcmp(type, "status") == 0) {
        if (callbacks_.on_status != nullptr) {
            callbacks_.on_status(json_str(doc.node, "persona", ""), json_str(doc.node, "state", ""),
                                 json_u32(doc.node, "tokens_in"), json_u32(doc.node, "tokens_out"),
                                 callbacks_.ctx);
        }
    } else if (std::strcmp(type, "vision_result") == 0) {
        if (callbacks_.on_vision_result != nullptr) {
            const cJSON *conf = cJSON_GetObjectItemCaseSensitive(doc.node, "confidence");
            float confidence = cJSON_IsNumber(conf) ? static_cast<float>(conf->valuedouble) : 0.0f;
            callbacks_.on_vision_result(json_str(doc.node, "summary"), confidence, callbacks_.ctx);
        }
    } else if (std::strcmp(type, "tts") == 0) {
        if (callbacks_.on_tts_ready != nullptr) {
            callbacks_.on_tts_ready(json_str(doc.node, "url"), callbacks_.ctx);
        }
    } else if (std::strcmp(type, "log") == 0 || std::strcmp(type, "error") == 0) {
        if (callbacks_.on_log_line != nullptr) {
            callbacks_.on_log_line(json_str(doc.node, "text"), callbacks_.ctx);
        }
    } else if (std::strcmp(type, "tasks") == 0) {
        if (callbacks_.on_tasks != nullptr) {
            HermesTaskItem items[kMaxTaskItems] = {};
            int count = 0;
            const cJSON *arr = cJSON_GetObjectItemCaseSensitive(doc.node, "items");
            const cJSON *it = nullptr;
            cJSON_ArrayForEach(it, arr) {
                if (count >= kMaxTaskItems) {
                    break;
                }
                const char *label = json_str(it, "label");
                if (label[0] == '\0') {
                    continue;
                }
                strlcpy(items[count].label, label, sizeof(items[count].label));
                const cJSON *done = cJSON_GetObjectItemCaseSensitive(it, "done");
                items[count].done = cJSON_IsTrue(done);
                ++count;
            }
            callbacks_.on_tasks(items, count, callbacks_.ctx);
        }
    } else {
        ESP_LOGD(TAG, "ignoring envelope type '%s'", type);
    }
}

void WsHermesClient::event_handler(void *arg, const char *, int32_t event_id, void *event_data)
{
    auto *self = static_cast<WsHermesClient *>(arg);
    auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to configured gateway");
        self->connected_.store(true, std::memory_order_release);
        self->send_hello();
        if (self->callbacks_.on_link != nullptr) {
            self->callbacks_.on_link(true, self->callbacks_.ctx);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (self->connected_ && self->callbacks_.on_link != nullptr) {
            self->callbacks_.on_link(false, self->callbacks_.ctx);
        }
        self->connected_.store(false, std::memory_order_release);
        self->rx_assembly_.clear();
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data == nullptr || data->op_code == 0x09 || data->op_code == 0x0A) {
            break;  // ping/pong
        }
        if (data->data_len > 0) {
            self->rx_bytes_.fetch_add(static_cast<uint32_t>(data->data_len), std::memory_order_relaxed);
            self->rx_assembly_.append(data->data_ptr, data->data_len);
        }
        // payload_offset+data_len == payload_len marks the end of one WS
        // message even when the client splits it across DATA events.
        if (data->payload_offset + data->data_len >= data->payload_len) {
            if (!self->rx_assembly_.empty()) {
                self->handle_text_frame(self->rx_assembly_.data(), self->rx_assembly_.size());
                self->rx_assembly_.clear();
            }
        }
        break;
    default:
        break;
    }
}
