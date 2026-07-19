#include "profile_import.hpp"
#include "app_config.hpp"

#include "esp_log.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const char *TAG = "profile_import";

static std::string trim(std::string value)
{
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }

    return value.substr(start, end - start);
}

static std::string unquote(std::string value)
{
    value = trim(value);
    if (value.size() >= 2) {
        char first = value.front();
        char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

static bool set_if_changed(std::string &field, const std::string &value)
{
    if (field == value) {
        return false;
    }
    field = value;
    return true;
}

static std::string normalize_url_value(const std::string &value)
{
    // Placeholder values from the template stay "unset" so the wizard runs.
    if (value == "CHANGE_ME" || value == "wss://your-server.example.com/ws/tab5") {
        return {};
    }
    return value;
}

esp_err_t import_profile_file(const char *path, WifiProfile &wifi, HermesProfile &hermes,
                              bool &changed, bool skip_wifi)
{
    changed = false;

    FILE *file = std::fopen(path, "r");
    if (file == nullptr) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    char line[384];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        std::string raw(line);
        raw = trim(raw);
        if (raw.empty() || raw[0] == '#' || raw[0] == ';') {
            continue;
        }

        size_t equals = raw.find('=');
        if (equals == std::string::npos) {
            ESP_LOGW(TAG, "Ignoring malformed profile line");
            continue;
        }

        std::string key = trim(raw.substr(0, equals));
        std::string value = unquote(raw.substr(equals + 1));

        if (key == "wifi_ssid" || key == "wifi_password") {
            if (skip_wifi) {
                continue;  // on-device settings own Wi-Fi (override flag set)
            }
            if (key == "wifi_ssid") {
                changed |= set_if_changed(wifi.ssid, value);
            } else {
                changed |= set_if_changed(wifi.password, value);
            }
        } else if (key == "gateway_ws_url") {
            changed |= set_if_changed(hermes.gateway_ws_url, normalize_url_value(value));
        } else if (key == "gateway_token") {
            changed |= set_if_changed(hermes.gateway_token, value);
        } else if (key == "persona") {
            changed |= set_if_changed(hermes.persona, value);
        } else if (key == "boot_mode") {
            changed |= set_if_changed(hermes.boot_mode, value == "setup" ? "setup" : "console");
        } else if (key == "screen_brightness") {
            char *end = nullptr;
            long brightness = std::strtol(value.c_str(), &end, 10);
            if (end != value.c_str() && *end == '\0' && brightness >= 10 && brightness <= 100) {
                if (hermes.screen_brightness != static_cast<int>(brightness)) {
                    hermes.screen_brightness = static_cast<int>(brightness);
                    changed = true;
                }
            } else {
                ESP_LOGW(TAG, "ignoring invalid screen_brightness value");
            }
        } else if (key == "speaker_volume") {
            char *end = nullptr;
            long volume = std::strtol(value.c_str(), &end, 10);
            if (end != value.c_str() && *end == '\0' && volume >= 0 && volume <= 100) {
                if (hermes.speaker_volume != static_cast<int>(volume)) {
                    hermes.speaker_volume = static_cast<int>(volume);
                    changed = true;
                }
            } else {
                ESP_LOGW(TAG, "ignoring invalid speaker_volume value");
            }
        } else {
            ESP_LOGW(TAG, "Ignoring unknown profile key '%s'", key.c_str());
        }
    }

    std::fclose(file);
    return ESP_OK;
}
