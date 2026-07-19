#pragma once

#include "profile_store.hpp"

#include "lvgl.h"

#include <cstddef>
#include <cstdint>
#include <string>

// On-device settings for the Archie console: gateway URL/token, Wi-Fi
// credentials, persona, boot mode and screen brightness — no SD card edits
// needed. Tap a row to select it, type on the physical keyboard, Enter
// commits, ESC cancels (or closes the screen when nothing is being edited).
// SAVE persists to NVS; gateway/Wi-Fi changes take effect on the next boot
// (REBOOT button). SETUP WIZARD reopens the full provider/API/voice flow.
//
// NOTE: fields the SD profile file defines (e.g. wifi_ssid) are re-imported
// at every boot, so SD wins over NVS for those on this device. The gateway
// keys are absent from the SD file, so values saved here stick.
//
// All methods run on the LVGL task under bsp_display_lock(), same contract
// as the console screen.
class SettingsScreen {
public:
    SettingsScreen() = default;
    ~SettingsScreen()
    {
        if (screen_ != nullptr) {
            lv_obj_delete(screen_);
        }
    }
    // Holds non-owning profile pointers and LVGL objects: not copyable.
    SettingsScreen(const SettingsScreen &) = delete;
    SettingsScreen &operator=(const SettingsScreen &) = delete;

    // reboot=true means the user asked for SAVE+REBOOT; the owner restarts.
    using CloseCallback = void (*)(bool reboot, void *ctx);

    void create(ProfileStore *store, WifiProfile *wifi, HermesProfile *hermes,
                CloseCallback on_close, void *ctx);
    bool created() const { return screen_ != nullptr; }

    // Optional: live-apply hook for the VOLUME row (agent_os passes the
    // speaker codec setter; terminal mode persists only).
    using VolumeApply = void (*)(int percent, void *ctx);
    void set_volume_apply(VolumeApply apply, void *ctx)
    {
        volume_apply_ = apply;
        volume_apply_ctx_ = ctx;
    }
    lv_obj_t *screen() { return screen_; }

    // Marks the settings screen as the active key-input consumer. The owner
    // loads the screen object itself.
    void set_active(bool active) { active_ = active; }
    bool active() const { return active_; }

    void key_input(const uint8_t *data, size_t len);

private:
    enum Field : int {
        FIELD_GATEWAY_URL = 0,
        FIELD_GATEWAY_TOKEN,
        FIELD_WIFI_SSID,
        FIELD_WIFI_PASS,
        FIELD_PERSONA,
        FIELD_BOOT_MODE,
        FIELD_COUNT,
    };

    void build_rows(lv_obj_t *parent);
    void select_field(int field);
    void commit_edit();
    void cancel_edit();
    void refresh_row(int field);
    void refresh_edit_line();
    void cycle_brightness();
    void apply_brightness(int percent);
    void cycle_volume();
    void save_to_nvs();
    void request_close(bool reboot);
    std::string field_value(int field) const;
    std::string display_value(int field) const;
    void set_field_value(int field, const std::string &value);
    void flash_status(const char *text, uint32_t color);

    static void row_event_cb(lv_event_t *e);
    static void button_event_cb(lv_event_t *e);

    ProfileStore *store_ = nullptr;
    WifiProfile *wifi_ = nullptr;
    HermesProfile *hermes_ = nullptr;
    CloseCallback on_close_ = nullptr;
    void *close_ctx_ = nullptr;

    lv_obj_t *screen_ = nullptr;
    lv_obj_t *rows_[FIELD_COUNT] = {};
    lv_obj_t *row_values_[FIELD_COUNT] = {};
    lv_obj_t *brightness_value_ = nullptr;
    lv_obj_t *volume_value_ = nullptr;
    lv_obj_t *edit_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    VolumeApply volume_apply_ = nullptr;
    void *volume_apply_ctx_ = nullptr;

    int selected_field_ = -1;  // -1 = nothing being edited
    std::string edit_buffer_;
    bool active_ = false;
    bool wifi_edited_ = false;  // a Wi-Fi field was committed this session
};
