#pragma once

#if __has_include("esp_err.h")
#include "esp_err.h"
#else
using esp_err_t = int;
#endif
#include <string>

// Everything the console needs to come up: which gateway/provider to use,
// how to authenticate, and which persona fronts the conversation.
struct HermesProfile {
    // Connection preset: hermes, openclaw, openai, claude, or custom.
    // Hermes uses the small WebSocket adapter. OpenClaw uses its official
    // OpenAI-compatible HTTP endpoint; the remaining modes call a model API.
    std::string connection_mode = "hermes";
    // WebSocket endpoint, e.g. wss://your-server.example.com/ws/tab5
    std::string gateway_ws_url;
    // Shared secret presented in the hello envelope. Never logged, masked in UI.
    std::string gateway_token;
    // Direct-provider settings. API keys are stored only in device NVS and
    // are never compiled into firmware, printed, or committed.
    std::string api_base_url = "https://api.openai.com/v1";
    std::string api_key;
    std::string api_model = "gpt-5.6-terra";
    // Optional ElevenLabs voice configuration. Voice remains disabled unless
    // explicitly enabled and both key and voice ID are present.
    std::string elevenlabs_key;
    std::string elevenlabs_voice_id;
    bool voice_enabled = false;
    // Persona selected at boot: "hermes", "archie", "mira" or a custom name.
    std::string persona = "archie";
    // "console" boots straight into the command centre; "setup" always shows
    // the first-run wizard (useful for kiosk/demo units).
    std::string boot_mode = "console";
    // Backlight percent (10-100), editable from the on-device settings screen.
    int screen_brightness = 100;
    // Speaker volume percent (0-100); 60 keeps the small driver clean.
    int speaker_volume = 60;
};

struct WifiProfile {
    std::string ssid;
    std::string password;
};

// True once the wizard (or SD import / settings screen) has provided the two
// essentials. Until then boot lands in the setup wizard.
inline bool profile_is_configured(const WifiProfile &wifi, const HermesProfile &hermes)
{
    if (wifi.ssid.empty()) {
        return false;
    }
    if (hermes.connection_mode == "hermes") {
        return !hermes.gateway_ws_url.empty() && !hermes.gateway_token.empty();
    }
    const bool key_present = hermes.connection_mode == "custom" || !hermes.api_key.empty();
    return !hermes.api_base_url.empty() && key_present && !hermes.api_model.empty();
}

class ProfileStore {
public:
    esp_err_t begin();
    esp_err_t load(WifiProfile &wifi, HermesProfile &hermes);
    esp_err_t save(const WifiProfile &wifi, const HermesProfile &hermes);

    // When set, Wi-Fi credentials saved from the on-device settings screen
    // take precedence: the boot-time SD profile import skips wifi_ssid /
    // wifi_password. Cleared by saving an empty SSID (SD resumes control).
    esp_err_t load_wifi_override(bool &enabled);
    esp_err_t save_wifi_override(bool enabled);

private:
    bool initialized_ = false;
};
