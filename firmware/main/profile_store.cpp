#include "profile_store.hpp"
#include "app_config.hpp"

#include "esp_log.h"
#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "profile_store";

esp_err_t ProfileStore::begin()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS");
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        initialized_ = true;
    }
    return err;
}

static std::string read_string(nvs_handle_t handle, const char *key)
{
    size_t len = 0;
    if (nvs_get_str(handle, key, nullptr, &len) != ESP_OK || len == 0) {
        return {};
    }

    std::string value(len, '\0');
    if (nvs_get_str(handle, key, value.data(), &len) != ESP_OK) {
        return {};
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

esp_err_t ProfileStore::load(WifiProfile &wifi, HermesProfile &hermes)
{
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Fresh device: defaults route boot into the setup wizard.
        wifi = {};
        hermes = {};
        hermes.gateway_ws_url = DEFAULT_GATEWAY_WS_URL;
        hermes.gateway_token = DEFAULT_GATEWAY_TOKEN;
        hermes.persona = DEFAULT_PERSONA;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open NVS profile namespace");

    wifi.ssid = read_string(handle, "wifi_ssid");
    wifi.password = read_string(handle, "wifi_pass");
    hermes.connection_mode = read_string(handle, "conn_mode");
    hermes.gateway_ws_url = read_string(handle, "gw_url");
    hermes.gateway_token = read_string(handle, "gw_token");
    hermes.api_base_url = read_string(handle, "api_base");
    hermes.api_key = read_string(handle, "api_key");
    hermes.api_model = read_string(handle, "api_model");
    hermes.elevenlabs_key = read_string(handle, "eleven_key");
    hermes.elevenlabs_voice_id = read_string(handle, "eleven_voice");
    uint8_t voice_enabled = 0;
    nvs_get_u8(handle, "voice_on", &voice_enabled);
    hermes.voice_enabled = voice_enabled != 0;
    hermes.persona = read_string(handle, "persona");
    hermes.boot_mode = read_string(handle, "boot_mode");
    int32_t brightness = 100;
    nvs_get_i32(handle, "brightness", &brightness);
    hermes.screen_brightness = brightness < 10 || brightness > 100 ? 100 : static_cast<int>(brightness);
    int32_t volume = 60;
    nvs_get_i32(handle, "volume", &volume);
    hermes.speaker_volume = volume < 0 || volume > 100 ? 60 : static_cast<int>(volume);

    if (hermes.gateway_ws_url.empty()) {
        hermes.gateway_ws_url = DEFAULT_GATEWAY_WS_URL;
    }
    if (hermes.gateway_token.empty()) {
        hermes.gateway_token = DEFAULT_GATEWAY_TOKEN;
    }
    if (hermes.persona.empty()) {
        hermes.persona = DEFAULT_PERSONA;
    }
    if (hermes.connection_mode != "openclaw" && hermes.connection_mode != "openai" &&
        hermes.connection_mode != "claude" && hermes.connection_mode != "custom") {
        hermes.connection_mode = "hermes";
    }
    if (hermes.api_base_url.empty()) {
        hermes.api_base_url = hermes.connection_mode == "claude"
                                  ? "https://api.anthropic.com"
                              : hermes.connection_mode == "openclaw"
                                  ? "http://openclaw-host.local:18789/v1"
                                  : "https://api.openai.com/v1";
    }
    if (hermes.api_model.empty()) {
        hermes.api_model = hermes.connection_mode == "claude"
                               ? "claude-sonnet-5"
                           : hermes.connection_mode == "openclaw"
                               ? "openclaw/default"
                               : "gpt-5.6-terra";
    }
    if (hermes.boot_mode != "setup") {
        hermes.boot_mode = "console";
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t ProfileStore::save(const WifiProfile &wifi, const HermesProfile &hermes)
{
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(APP_NAMESPACE, NVS_READWRITE, &handle), TAG, "open NVS profile namespace");

    esp_err_t err = ESP_OK;
    err |= nvs_set_str(handle, "wifi_ssid", wifi.ssid.c_str());
    err |= nvs_set_str(handle, "wifi_pass", wifi.password.c_str());
    err |= nvs_set_str(handle, "conn_mode", hermes.connection_mode.c_str());
    err |= nvs_set_str(handle, "gw_url", hermes.gateway_ws_url.c_str());
    err |= nvs_set_str(handle, "gw_token", hermes.gateway_token.c_str());
    err |= nvs_set_str(handle, "api_base", hermes.api_base_url.c_str());
    err |= nvs_set_str(handle, "api_key", hermes.api_key.c_str());
    err |= nvs_set_str(handle, "api_model", hermes.api_model.c_str());
    err |= nvs_set_str(handle, "eleven_key", hermes.elevenlabs_key.c_str());
    err |= nvs_set_str(handle, "eleven_voice", hermes.elevenlabs_voice_id.c_str());
    err |= nvs_set_u8(handle, "voice_on", hermes.voice_enabled ? 1 : 0);
    err |= nvs_set_str(handle, "persona", hermes.persona.c_str());
    err |= nvs_set_str(handle, "boot_mode", hermes.boot_mode.c_str());
    int32_t brightness = hermes.screen_brightness;
    if (brightness < 10 || brightness > 100) {
        brightness = 100;
    }
    err |= nvs_set_i32(handle, "brightness", brightness);
    int32_t volume = hermes.speaker_volume;
    if (volume < 0 || volume > 100) {
        volume = 60;
    }
    err |= nvs_set_i32(handle, "volume", volume);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t ProfileStore::load_wifi_override(bool &enabled)
{
    enabled = false;
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(APP_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    uint8_t value = 0;
    nvs_get_u8(handle, "wifi_override", &value);
    enabled = value != 0;
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t ProfileStore::save_wifi_override(bool enabled)
{
    if (!initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle = 0;
    ESP_RETURN_ON_ERROR(nvs_open(APP_NAMESPACE, NVS_READWRITE, &handle), TAG, "open NVS wifi override");
    esp_err_t err = nvs_set_u8(handle, "wifi_override", enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
