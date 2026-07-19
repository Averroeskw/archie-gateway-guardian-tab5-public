#include "hermes_mode.hpp"

#include "app_config.hpp"
#include "direct_ai_client.hpp"
#include "elevenlabs_voice.hpp"
#include "hermes_console_screen.hpp"
#include "hermes_theme.hpp"
#include "settings_screen.hpp"
#include "telemetry_model.hpp"
#include "ws_hermes_client.hpp"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

// Hermes command centre runner: a text-first console that talks to the
// gateway over WebSocket. Chat is the whole surface.

static const char *TAG = "hermes_mode";

static HermesConsoleScreen s_console;
static SettingsScreen s_settings;
static TelemetryModel s_telemetry;
static WsHermesClient s_ws;
static DirectAiClient s_direct;
static ElevenLabsVoice s_voice;
static bool s_direct_mode = false;
static bool s_voice_enabled = false;
static std::string s_reply_text;
static std::atomic<bool> s_active{false};
static ProfileStore *s_store = nullptr;
static WifiProfile *s_wifi_profile = nullptr;
static HermesProfile *s_hermes_profile = nullptr;

// All LVGL mutations go under bsp_display_lock(0) — the hard-won lesson
// applies unchanged: a timed-out lock that skips work corrupts state, and
// blocking on a producer task is harmless.

static void console_log(const char *line, uint32_t color)
{
    if (bsp_display_lock(0)) {
        s_console.append_log(line, color);
        bsp_display_unlock();
    }
}

// ---- WS callbacks (fire on the websocket client task) ----------------------

static void ws_on_chat_text(const char *persona, const char *text, bool done, void *)
{
    if (text != nullptr && text[0] != '\0') {
        s_reply_text += text;
    }
    if (bsp_display_lock(0)) {
        s_console.on_chat_text(persona, text, done);
        bsp_display_unlock();
    }
    if (done) {
        if (s_voice_enabled && !s_reply_text.empty()) {
            if (s_voice.speak(s_reply_text.c_str()) != ESP_OK) {
                console_log("voice: busy; reply stayed text-only", hermes_theme::COL_DIM);
            }
        }
        s_reply_text.clear();
        s_telemetry.set_agent(nullptr, "idle");
    }
}

static void ws_on_status(const char *persona, const char *state, uint32_t tokens_in, uint32_t tokens_out,
                         void *)
{
    ESP_LOGI(TAG, "status: %s/%s in=%lu out=%lu", persona, state,
             static_cast<unsigned long>(tokens_in), static_cast<unsigned long>(tokens_out));
    s_telemetry.set_agent(persona, state);
    s_telemetry.set_tokens(tokens_in, tokens_out);
}

static void ws_on_log_line(const char *line, void *)
{
    console_log(line, hermes_theme::COL_DIM);
}

static void ws_on_tasks(const HermesTaskItem *items, int count, void *)
{
    if (bsp_display_lock(0)) {
        s_console.set_tasks(items, count);
        bsp_display_unlock();
    }
}

// First-connect self-greeting: proves the full send -> agent -> reply loop
// with no user input. Sent from the LVGL task (a SAFE context) — sending
// from inside the WS event callback is reentrant and gets dropped, which is
// what made device-originated requests silently vanish.
static std::atomic<bool> s_greeted{false};
static std::atomic<bool> s_greet_pending{false};

static void ws_on_link(bool connected, void *)
{
    s_telemetry.set_gateway(connected, nullptr);
    console_log(connected ? "gateway link UP" : "gateway link DOWN",
                connected ? hermes_theme::COL_YELLOW : hermes_theme::COL_ALERT);
    if (connected && !s_direct_mode && !s_greeted.exchange(true)) {
        s_greet_pending = true;
    }
}

// ---- Console actions (fire on the LVGL task, display lock already held) -----

static void action_send_chat(const char *persona, const char *text, void *)
{
    s_telemetry.set_agent(persona, "thinking");
    esp_err_t err = s_direct_mode ? s_direct.send_chat(persona, text)
                                  : s_ws.send_chat(persona, text);
    if (err != ESP_OK) {
        s_console.append_log("send failed: transport unavailable", hermes_theme::COL_ALERT);
        s_telemetry.set_agent(persona, "error");
    }
}

static void action_stop(void *)
{
    if (s_direct_mode) {
        s_direct.send_stop();
    } else {
        s_ws.send_stop();
    }
    s_telemetry.set_agent(nullptr, "idle");
}

static void action_clear_session(const char *persona, void *)
{
    // Best-effort: offline is fine, the gateway resets context on reconnect
    // anyway (memory is per-connection).
    if (s_direct_mode) {
        s_direct.send_clear(persona);
    } else {
        s_ws.send_clear(persona);
    }
}

// ---- Settings ---------------------------------------------------------------

static void settings_closed(bool reboot, void *)
{
    if (reboot) {
        ESP_LOGI(TAG, "settings requested reboot");
        esp_restart();
    }
    s_settings.set_active(false);
    // Both screens are retained and reused on every settings round-trip.
    lv_screen_load(s_console.screen());
}

static void action_open_settings(void *)
{
    if (!s_settings.created()) {
        if (s_store == nullptr || s_wifi_profile == nullptr || s_hermes_profile == nullptr) {
            s_console.append_log("settings unavailable (no profile store)", hermes_theme::COL_ALERT);
            return;
        }
        s_settings.create(s_store, s_wifi_profile, s_hermes_profile, settings_closed, nullptr);
        s_settings.set_volume_apply([](int percent, void *) { s_voice.set_volume(percent); },
                                    nullptr);
    }
    s_settings.set_active(true);
    lv_scr_load(s_settings.screen());
}

// ---- Self-test (type /test) -------------------------------------------------

static void self_test_task(void *)
{
    console_log("SELF-TEST: battery / wifi / gateway / link", hermes_theme::COL_SUN);
    char line[120];

    s_telemetry.sample_device();
    TelemetrySnapshot snap = s_telemetry.snapshot();
    if (snap.battery_percent >= 0) {
        std::snprintf(line, sizeof(line), "TEST battery: %.2fV ~%d%% -> PASS",
                      static_cast<double>(snap.battery_volts), snap.battery_percent);
        console_log(line, hermes_theme::COL_YELLOW);
    } else {
        console_log("TEST battery: no INA226 reading (USB-only is fine)", hermes_theme::COL_DIM);
    }

    if (snap.wifi_connected) {
        // Glyph instead of the IP: keeps the visible log free of private info.
        std::snprintf(line, sizeof(line), "TEST wifi: ◈ @ %ddBm -> PASS", snap.wifi_rssi_dbm);
        console_log(line, hermes_theme::COL_YELLOW);
    } else {
        console_log("TEST wifi: NOT CONNECTED -> check SET", hermes_theme::COL_ALERT);
    }

    bool linked = s_direct_mode ? s_direct.connected() : s_ws.connected();
    console_log(linked ? "TEST transport: ready -> PASS"
                       : "TEST transport: OFFLINE (check SET)",
                linked ? hermes_theme::COL_YELLOW : hermes_theme::COL_ALERT);

    if (linked) {
        const char *persona = s_console.active_persona();
        std::snprintf(line, sizeof(line), "TEST chat: pinging %s...", persona);
        console_log(line, hermes_theme::COL_DIM);
        s_telemetry.set_agent(persona, "thinking");
        esp_err_t send_err = s_direct_mode
                                 ? s_direct.send_chat(persona, "Reply with exactly: command centre online.")
                                 : s_ws.send_chat(persona, "Reply with exactly: command centre online.");
        if (send_err != ESP_OK) {
            console_log("TEST chat: send failed", hermes_theme::COL_ALERT);
            s_telemetry.set_agent(persona, "idle");
        }
    }
    console_log("SELF-TEST done", hermes_theme::COL_SUN);
    vTaskDelete(nullptr);
}

static void action_run_self_test(void *)
{
    if (xTaskCreate(self_test_task, "self_test", 4096, nullptr, 3, nullptr) != pdPASS) {
        s_console.append_log("self-test: task spawn failed", hermes_theme::COL_ALERT);
    }
}

// ---- Periodic refresh -------------------------------------------------------

// LVGL timer at 10Hz: send any pending greeting from this safe context, then
// pull one telemetry snapshot and repaint only changed widgets.
static void console_refresh_cb(lv_timer_t *)
{
    if (s_greet_pending.exchange(false)) {
        const char *persona = s_console.active_persona();
        s_telemetry.set_agent(persona, "thinking");
        s_console.append_log("self-greeting: confirming the link...", hermes_theme::COL_DIM);
        if (s_ws.send_chat(persona, "In one short sentence, confirm you are online.") != ESP_OK) {
            s_console.append_log("self-greeting send failed", hermes_theme::COL_ALERT);
            s_telemetry.set_agent(persona, "idle");
        }
    }
    TelemetrySnapshot snap = s_telemetry.snapshot();
    snap.ws_rx_bytes = s_direct_mode ? s_direct.rx_bytes() : s_ws.rx_bytes();
    snap.ws_tx_bytes = s_direct_mode ? s_direct.tx_bytes() : s_ws.tx_bytes();
    s_console.update_telemetry(snap);
}

// 1Hz device sampler: heap, PSRAM, uptime, battery (I2C), Wi-Fi RSSI/IP.
static void sampler_task(void *)
{
    for (;;) {
        s_telemetry.sample_device();
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            esp_netif_ip_info_t ip_info = {};
            char ip[20] = "";
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                std::snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
            }
            s_telemetry.set_wifi(true, ap.rssi, ip);
        } else {
            s_telemetry.set_wifi(false, 0, "");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---- Entry ------------------------------------------------------------------

esp_err_t hermes_mode_start(ProfileStore *store, WifiProfile *wifi, HermesProfile *hermes)
{
    if (store == nullptr || wifi == nullptr || hermes == nullptr) {
        ESP_LOGE(TAG, "profile store and profiles are required");
        return ESP_ERR_INVALID_ARG;
    }
    s_store = store;
    s_wifi_profile = wifi;
    s_hermes_profile = hermes;
    const HermesProfile &profile = *hermes;
    s_direct_mode = profile.connection_mode != "hermes";
    s_voice_enabled = false;
    s_reply_text.clear();
    s_greeted = false;
    s_greet_pending = false;
    s_telemetry.begin();

    std::string uri = s_direct_mode ? profile.api_base_url
                                    : (profile.gateway_ws_url.empty() ? DEFAULT_GATEWAY_WS_URL
                                                                     : profile.gateway_ws_url);
    std::string token = profile.gateway_token.empty() ? DEFAULT_GATEWAY_TOKEN : profile.gateway_token;
    s_telemetry.set_gateway(false, s_direct_mode ? profile.connection_mode.c_str() : uri.c_str());

    ConsoleActions actions = {
        .send_chat = action_send_chat,
        .stop_generation = action_stop,
        .open_settings = action_open_settings,
        .run_self_test = action_run_self_test,
        .clear_session = action_clear_session,
        .ctx = nullptr,
    };
    // 0 = wait forever in esp_lvgl_port (NOT non-blocking); the false branch
    // is unreachable in practice but must not leave the mode half-active.
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "display lock unavailable; console not created");
        return ESP_FAIL;
    }
    s_console.create(bsp_display_get_input_dev(), actions);
    s_console.set_persona(profile.persona.c_str());
    // First entry replaces the splash or setup test. Delete that old tree
    // immediately; it can own a PSRAM point-cloud buffer and boot animations.
    lv_screen_load_anim(s_console.screen(), LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
    lv_timer_create(console_refresh_cb, 100, nullptr);
    bsp_display_unlock();
    s_active = true;
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        bsp_display_brightness_set(std::clamp(profile.screen_brightness, 10, 100)));

    if (profile.voice_enabled && !profile.elevenlabs_key.empty() &&
        !profile.elevenlabs_voice_id.empty()) {
        ElevenLabsVoiceConfig voice_config = {
            .api_key = profile.elevenlabs_key,
            .voice_id = profile.elevenlabs_voice_id,
            .volume = profile.speaker_volume,
        };
        esp_err_t voice_err = s_voice.begin(
            voice_config,
            [](const char *line, bool error, void *) {
                console_log(line, error ? hermes_theme::COL_ALERT : hermes_theme::COL_DIM);
            },
            nullptr);
        s_voice_enabled = voice_err == ESP_OK;
        console_log(s_voice_enabled ? "voice core ready" : "voice core unavailable",
                    s_voice_enabled ? hermes_theme::COL_DIM : hermes_theme::COL_ALERT);
    } else if (profile.voice_enabled) {
        console_log("voice disabled: ElevenLabs key + voice ID required",
                    hermes_theme::COL_ALERT);
    }

    // The device owns optional ElevenLabs synthesis, so gateway TTS envelopes
    // stay unused and no ElevenLabs credential is disclosed to the gateway.
    esp_err_t err;
    if (s_direct_mode) {
        DirectAiCallbacks callbacks = {
            .on_chat_text = ws_on_chat_text,
            .on_status = ws_on_status,
            .on_link = ws_on_link,
            .on_log_line = ws_on_log_line,
            .ctx = nullptr,
        };
        DirectAiConfig direct_config = {
            .provider = profile.connection_mode,
            .base_url = profile.api_base_url,
            .api_key = profile.api_key,
            .model = profile.api_model,
        };
        err = s_direct.begin(direct_config, callbacks);
    } else {
        WsHermesCallbacks callbacks = {
            .on_chat_text = ws_on_chat_text,
            .on_status = ws_on_status,
            .on_vision_result = nullptr,
            .on_tts_ready = nullptr,
            .on_link = ws_on_link,
            .on_log_line = ws_on_log_line,
            .on_tasks = ws_on_tasks,
            .ctx = nullptr,
        };
        WsHermesConfig ws_config = {
            .uri = uri,
            .token = token,
            .backend = profile.connection_mode,
        };
        err = s_ws.begin(ws_config, callbacks);
    }
    if (err != ESP_OK) {
        console_log("transport failed to start", hermes_theme::COL_ALERT);
    } else {
        // Sigil instead of the WSS URL: the gateway endpoint never hits the
        // visible log, so the screen is safe to photograph.
        console_log(s_direct_mode ? "provider ready ═══ ◈ ═══" : "connecting ═══ ◈ ═══",
                    hermes_theme::COL_DIM);
    }

    // (SNTP + TZ are initialized once in app_main, before the wizard's TLS
    // test can need a valid clock.)

    if (xTaskCreate(sampler_task, "telem_sample", 4096, nullptr, 3, nullptr) != pdPASS) {
        console_log("telemetry sampler failed to start", hermes_theme::COL_ALERT);
    }
    ESP_LOGI(TAG, "command centre running (%s mode)", profile.connection_mode.c_str());
    return err;
}

bool hermes_mode_active()
{
    return s_active.load();
}

void hermes_mode_key_input(const uint8_t *data, size_t len)
{
    if (bsp_display_lock(0)) {
        if (s_settings.active()) {
            s_settings.key_input(data, len);
        } else {
            s_console.key_input(data, len);
        }
        bsp_display_unlock();
    }
}
