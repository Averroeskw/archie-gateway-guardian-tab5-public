#include "improv_serial.hpp"

#include "app_config.hpp"
#include "wifi_manager.hpp"

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Improv Serial v1 (https://www.improv-wifi.com/serial/).
// Frame: "IMPROV" + 0x01 + type + len + payload + checksum(low byte of sum).

namespace {

const char *TAG = "improv";

constexpr uint8_t kVersion = 0x01;

enum FrameType : uint8_t {
    TYPE_CURRENT_STATE = 0x01,
    TYPE_ERROR_STATE = 0x02,
    TYPE_RPC_COMMAND = 0x03,
    TYPE_RPC_RESULT = 0x04,
};

enum State : uint8_t {
    STATE_READY = 0x02,        // authorized, awaiting credentials
    STATE_PROVISIONING = 0x03,
    STATE_PROVISIONED = 0x04,
};

enum Error : uint8_t {
    ERR_NONE = 0x00,
    ERR_INVALID_PACKET = 0x01,
    ERR_UNKNOWN_COMMAND = 0x02,
    ERR_UNABLE_TO_CONNECT = 0x03,
};

enum RpcCommand : uint8_t {
    RPC_WIFI_SETTINGS = 0x01,
    RPC_GET_CURRENT_STATE = 0x02,
    RPC_GET_DEVICE_INFO = 0x03,
    RPC_GET_WIFI_NETWORKS = 0x04,
    RPC_SET_STATIC_LEASE = 0x7A,  // vendor extension, see header
};

// Pull `count` length-prefixed strings out of an RPC body. Returns false on
// truncation.
bool parse_strings(const uint8_t *body, uint8_t datalen, std::vector<std::string> &out, int count)
{
    size_t off = 0;
    for (int i = 0; i < count; ++i) {
        if (off >= datalen) {
            return false;
        }
        uint8_t len = body[off++];
        if (off + len > datalen) {
            return false;
        }
        out.emplace_back(reinterpret_cast<const char *>(body + off), len);
        off += len;
    }
    return true;
}

ImprovCredentialsCallback s_on_credentials = nullptr;
ImprovProvisionedQuery s_is_provisioned = nullptr;
void *s_ctx = nullptr;

void write_frame(uint8_t type, const uint8_t *payload, size_t len)
{
    uint8_t frame[10 + 256];
    if (len > 255) {
        return;
    }
    size_t n = 0;
    std::memcpy(frame, "IMPROV", 6);
    n = 6;
    frame[n++] = kVersion;
    frame[n++] = type;
    frame[n++] = static_cast<uint8_t>(len);
    std::memcpy(frame + n, payload, len);
    n += len;
    uint32_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        sum += frame[i];
    }
    frame[n++] = static_cast<uint8_t>(sum & 0xFF);
    frame[n++] = '\n';
    usb_serial_jtag_write_bytes(frame, n, pdMS_TO_TICKS(100));
}

void send_state(uint8_t state)
{
    write_frame(TYPE_CURRENT_STATE, &state, 1);
}

void send_error(uint8_t error)
{
    write_frame(TYPE_ERROR_STATE, &error, 1);
}

// RPC result payload: responding-command, datalen, then length-prefixed strings.
void send_rpc_result(uint8_t command, const std::vector<std::string> &strings)
{
    uint8_t payload[256];
    size_t n = 0;
    payload[n++] = command;
    size_t datalen_at = n++;
    for (const std::string &s : strings) {
        if (n + 1 + s.size() >= sizeof(payload)) {
            return;
        }
        payload[n++] = static_cast<uint8_t>(s.size());
        std::memcpy(payload + n, s.data(), s.size());
        n += s.size();
    }
    payload[datalen_at] = static_cast<uint8_t>(n - datalen_at - 1);
    write_frame(TYPE_RPC_RESULT, payload, n);
}

void handle_rpc(const uint8_t *data, size_t len)
{
    if (len < 2) {
        send_error(ERR_INVALID_PACKET);
        return;
    }
    uint8_t command = data[0];
    uint8_t datalen = data[1];
    if (static_cast<size_t>(datalen) + 2 > len) {
        send_error(ERR_INVALID_PACKET);
        return;
    }
    const uint8_t *body = data + 2;

    switch (command) {
    case RPC_WIFI_SETTINGS: {
        // body: ssid_len, ssid, pass_len, pass
        if (datalen < 1) {
            send_error(ERR_INVALID_PACKET);
            return;
        }
        uint8_t ssid_len = body[0];
        if (1 + ssid_len + 1 > datalen) {
            send_error(ERR_INVALID_PACKET);
            return;
        }
        uint8_t pass_len = body[1 + ssid_len];
        if (1 + ssid_len + 1 + pass_len > datalen) {
            send_error(ERR_INVALID_PACKET);
            return;
        }
        std::string ssid(reinterpret_cast<const char *>(body + 1), ssid_len);
        std::string pass(reinterpret_cast<const char *>(body + 2 + ssid_len), pass_len);
        // Confirm receipt without leaking an SSID into serial logs.
        ESP_LOGI(TAG, "Wi-Fi credentials received");
        send_state(STATE_PROVISIONING);
        bool ok = s_on_credentials != nullptr && s_on_credentials(ssid.c_str(), pass.c_str(), s_ctx);
        if (ok) {
            send_state(STATE_PROVISIONED);
            // Redirect URL result: none (no on-device web UI) — empty list.
            send_rpc_result(RPC_WIFI_SETTINGS, {});
        } else {
            send_state(STATE_READY);
            send_error(ERR_UNABLE_TO_CONNECT);
        }
        return;
    }
    case RPC_GET_CURRENT_STATE: {
        bool up = s_is_provisioned != nullptr && s_is_provisioned(s_ctx);
        send_state(up ? STATE_PROVISIONED : STATE_READY);
        return;
    }
    case RPC_GET_DEVICE_INFO:
        send_rpc_result(RPC_GET_DEVICE_INFO,
                        {"archie-gateway-guardian", ARCHIE_OS_VERSION, "ESP32-P4", "M5Stack Tab5"});
        return;
    case RPC_GET_WIFI_NETWORKS: {
        // Real scan: one RPC result per network (ssid, rssi, secured YES/NO),
        // then an empty terminator. Doubles as a field diagnostic — what the
        // C6 actually sees, band included.
        if (wifi_ensure_started() == ESP_OK) {
            wifi_scan_config_t sc = {};
            sc.show_hidden = true;
            if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
                uint16_t count = 0;
                esp_wifi_scan_get_ap_num(&count);
                if (count > 20) {
                    count = 20;
                }
                static wifi_ap_record_t recs[20];
                uint16_t got = count;
                if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
                    for (uint16_t i = 0; i < got; ++i) {
                        char rssi[8];
                        std::snprintf(rssi, sizeof(rssi), "%d", recs[i].rssi);
                        send_rpc_result(RPC_GET_WIFI_NETWORKS,
                                        {reinterpret_cast<const char *>(recs[i].ssid), rssi,
                                         recs[i].authmode == WIFI_AUTH_OPEN ? "NO" : "YES"});
                    }
                }
            }
        }
        send_rpc_result(RPC_GET_WIFI_NETWORKS, {});
        return;
    }
    case RPC_SET_STATIC_LEASE: {
        std::vector<std::string> parts;
        if (!parse_strings(body, datalen, parts, 4)) {
            send_error(ERR_INVALID_PACKET);
            return;
        }
        if (wifi_seed_lease_cache(parts[0].c_str(), parts[1].c_str(), parts[2].c_str(),
                                  parts[3].c_str()) == ESP_OK) {
            ESP_LOGI(TAG, "static lease cached for configured network");
            send_rpc_result(RPC_SET_STATIC_LEASE, {"OK"});
        } else {
            send_error(ERR_INVALID_PACKET);
        }
        return;
    }
    default:
        send_error(ERR_UNKNOWN_COMMAND);
        return;
    }
}

// Byte-stream parser: hunt for "IMPROV", then read header + payload + checksum.
void improv_task(void *)
{
    // Announce readiness once so ESP Web Tools detects Improv support quickly
    // after flashing.
    vTaskDelay(pdMS_TO_TICKS(300));
    send_state(s_is_provisioned != nullptr && s_is_provisioned(s_ctx) ? STATE_PROVISIONED
                                                                      : STATE_READY);

    uint8_t buf[64];
    uint8_t frame[10 + 256];
    size_t pos = 0;      // bytes matched of "IMPROV" header / frame fill
    size_t expect = 0;   // total frame size once the length byte is known
    for (;;) {
        int got = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(250));
        for (int i = 0; i < got; ++i) {
            uint8_t byte = buf[i];
            if (pos < 6) {
                if (byte == "IMPROV"[pos]) {
                    frame[pos++] = byte;
                } else {
                    pos = (byte == 'I') ? 1 : 0;  // restart, allowing overlap
                }
                continue;
            }
            frame[pos++] = byte;
            if (pos == 9) {
                // version, type, len known
                expect = 9 + frame[8] + 1;  // + payload + checksum
                if (frame[6] != kVersion || expect > sizeof(frame)) {
                    pos = 0;
                    continue;
                }
            }
            if (pos >= 9 && pos == expect) {
                uint32_t sum = 0;
                for (size_t b = 0; b < pos - 1; ++b) {
                    sum += frame[b];
                }
                if (static_cast<uint8_t>(sum & 0xFF) != frame[pos - 1]) {
                    send_error(ERR_INVALID_PACKET);
                } else if (frame[7] == TYPE_RPC_COMMAND) {
                    handle_rpc(frame + 9, frame[8]);
                }
                pos = 0;
            }
        }
    }
}

}  // namespace

esp_err_t improv_serial_start(ImprovCredentialsCallback on_credentials,
                              ImprovProvisionedQuery is_provisioned, void *ctx)
{
    s_on_credentials = on_credentials;
    s_is_provisioned = is_provisioned;
    s_ctx = ctx;

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = 512,
        .rx_buffer_size = 512,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE = already installed
        ESP_LOGW(TAG, "usb_serial_jtag driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreate(improv_task, "improv", 4096, nullptr, 3, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Improv Wi-Fi serial provisioning ready");
    return ESP_OK;
}
