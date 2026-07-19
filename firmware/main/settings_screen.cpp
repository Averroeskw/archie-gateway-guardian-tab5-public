#include "settings_screen.hpp"

#include "app_config.hpp"
#include "hermes_theme.hpp"

#include "bsp/esp-bsp.h"
#include "esp_log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace hermes_theme;

static const char *TAG = "settings";

static constexpr int kPanelX = 240;
static constexpr int kPanelW = LCD_W - 2 * kPanelX;
// 8 rows (6 fields + brightness + volume) must fit above the edit line and
// action deck on the 720px panel, hence the tighter row height.
static constexpr int kRowH = 50;
static constexpr int kBrightnessSteps[] = {20, 40, 60, 80, 100};

static const char *kFieldNames[] = {
    "GATEWAY URL",
    "GATEWAY TOKEN",
    "WIFI SSID",
    "WIFI PASSWORD",
    "PERSONA (hermes/archie/mira/...)",
    "BOOT MODE (console/setup)",
};

// Button ids for the bottom action row.
enum SettingsButton : intptr_t {
    BTN_SAVE = 1,
    BTN_REBOOT,
    BTN_SETUP,
    BTN_BACK,
    BTN_BRIGHTNESS,
    BTN_VOLUME,
};
static constexpr int kVolumeSteps[] = {30, 45, 60, 75, 90};

void SettingsScreen::create(ProfileStore *store, WifiProfile *wifi, HermesProfile *hermes,
                            CloseCallback on_close, void *ctx)
{
    if (store == nullptr || wifi == nullptr || hermes == nullptr) {
        ESP_LOGE(TAG, "create() called with null profile pointers");
        return;
    }
    store_ = store;
    wifi_ = wifi;
    hermes_ = hermes;
    on_close_ = on_close;
    close_ctx_ = ctx;

    hermes_theme::init();
    screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_clear_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen_);
    lv_label_set_text(title, "ARCHIE // NEXUS SETTINGS");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_text_font(title, font_mono(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *panel = make_panel(screen_, "CONFIG", kPanelX, 56,
                                 kPanelW, (FIELD_COUNT + 2) * (kRowH + 8) + 16);
    add_corner_brackets(panel);
    build_rows(panel);

    // Edit line: shows the field being edited + cursor.
    lv_obj_t *edit_row = lv_obj_create(screen_);
    lv_obj_set_pos(edit_row, kPanelX, 56 + (FIELD_COUNT + 2) * (kRowH + 8) + 32);
    lv_obj_set_size(edit_row, kPanelW, 40);
    lv_obj_set_style_bg_color(edit_row, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(edit_row, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_border_width(edit_row, 1, 0);
    lv_obj_set_style_radius(edit_row, 4, 0);
    lv_obj_set_style_pad_hor(edit_row, 10, 0);
    lv_obj_clear_flag(edit_row, LV_OBJ_FLAG_SCROLLABLE);
    edit_label_ = lv_label_create(edit_row);
    lv_obj_set_width(edit_label_, kPanelW - 24);
    lv_label_set_long_mode(edit_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(edit_label_, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(edit_label_, font_mono(), 0);
    lv_obj_align(edit_label_, LV_ALIGN_LEFT_MID, 0, 0);

    status_label_ = lv_label_create(screen_);
    lv_label_set_text(status_label_, "tap a row, type on the keyboard, Enter commits, ESC backs out");
    lv_obj_set_style_text_color(status_label_, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(status_label_, font_mono(), 0);
    lv_obj_align(status_label_, LV_ALIGN_BOTTOM_MID, 0, -86);

    // Action buttons.
    lv_obj_t *deck = lv_obj_create(screen_);
    lv_obj_set_size(deck, kPanelW, 56);
    lv_obj_align(deck, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_opa(deck, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(deck, 0, 0);
    lv_obj_set_style_pad_all(deck, 0, 0);
    lv_obj_set_flex_flow(deck, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(deck, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(deck, LV_OBJ_FLAG_SCROLLABLE);

    struct Spec {
        const char *text;
        uint32_t accent;
        SettingsButton id;
    };
    const Spec specs[] = {
        {"SAVE", COL_YELLOW, BTN_SAVE},
        {"SAVE+REBOOT", COL_SUN, BTN_REBOOT},
        {"SETUP WIZARD", COL_AMBER, BTN_SETUP},
        {"BACK", COL_DIM, BTN_BACK},
    };
    for (const Spec &spec : specs) {
        lv_obj_t *btn = make_deck_button(deck, spec.text, spec.accent);
        lv_obj_set_size(btn, 185, 48);
        lv_obj_add_event_cb(btn, button_event_cb, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(btn, reinterpret_cast<void *>(spec.id));
    }

    refresh_edit_line();
}

void SettingsScreen::build_rows(lv_obj_t *parent)
{
    for (int field = 0; field < FIELD_COUNT; ++field) {
        lv_obj_t *row = lv_obj_create(parent);
        rows_[field] = row;
        lv_obj_set_pos(row, 0, field * (kRowH + 8));
        lv_obj_set_size(row, kPanelW - 32, kRowH);
        lv_obj_set_style_bg_color(row, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_PANEL_EDGE), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_style_pad_ver(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_CLICKED, this);
        lv_obj_set_user_data(row, reinterpret_cast<void *>(static_cast<intptr_t>(field)));

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, kFieldNames[field]);
        lv_obj_set_style_text_color(name, lv_color_hex(COL_SUN), 0);
        lv_obj_set_style_text_font(name, font_mono(), 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *value = lv_label_create(row);
        row_values_[field] = value;
        lv_obj_set_width(value, kPanelW - 56);
        lv_label_set_long_mode(value, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(value, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_text_font(value, font_mono(), 0);
        lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        refresh_row(field);
    }

    // Brightness row: tap to cycle, applies immediately, persisted on SAVE.
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_pos(row, 0, FIELD_COUNT * (kRowH + 8));
    lv_obj_set_size(row, kPanelW - 32, kRowH);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, button_event_cb, LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(row, reinterpret_cast<void *>(BTN_BRIGHTNESS));

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, "BRIGHTNESS (tap to cycle)");
    lv_obj_set_style_text_color(name, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_text_font(name, font_mono(), 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
    brightness_value_ = lv_label_create(row);
    lv_obj_set_style_text_color(brightness_value_, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(brightness_value_, font_mono(), 0);
    lv_obj_align(brightness_value_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    char text[16];
    std::snprintf(text, sizeof(text), "%d%%", hermes_->screen_brightness);
    lv_label_set_text(brightness_value_, text);

    // Volume row: tap to cycle; applies to the speaker live in console mode.
    lv_obj_t *vol_row = lv_obj_create(parent);
    lv_obj_set_pos(vol_row, 0, (FIELD_COUNT + 1) * (kRowH + 8));
    lv_obj_set_size(vol_row, kPanelW - 32, kRowH);
    lv_obj_set_style_bg_color(vol_row, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_color(vol_row, lv_color_hex(COL_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(vol_row, 1, 0);
    lv_obj_set_style_radius(vol_row, 4, 0);
    lv_obj_set_style_pad_hor(vol_row, 10, 0);
    lv_obj_set_style_pad_ver(vol_row, 6, 0);
    lv_obj_clear_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(vol_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(vol_row, button_event_cb, LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(vol_row, reinterpret_cast<void *>(BTN_VOLUME));

    lv_obj_t *vol_name = lv_label_create(vol_row);
    lv_label_set_text(vol_name, "SPEAKER VOLUME (tap to cycle)");
    lv_obj_set_style_text_color(vol_name, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_text_font(vol_name, font_mono(), 0);
    lv_obj_align(vol_name, LV_ALIGN_TOP_LEFT, 0, 0);
    volume_value_ = lv_label_create(vol_row);
    lv_obj_set_style_text_color(volume_value_, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(volume_value_, font_mono(), 0);
    lv_obj_align(volume_value_, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    std::snprintf(text, sizeof(text), "%d%%", hermes_->speaker_volume);
    lv_label_set_text(volume_value_, text);
}

std::string SettingsScreen::field_value(int field) const
{
    switch (field) {
    case FIELD_GATEWAY_URL:
        return hermes_->gateway_ws_url;
    case FIELD_GATEWAY_TOKEN:
        return hermes_->gateway_token;
    case FIELD_WIFI_SSID:
        return wifi_->ssid;
    case FIELD_WIFI_PASS:
        return wifi_->password;
    case FIELD_PERSONA:
        return hermes_->persona;
    case FIELD_BOOT_MODE:
        return hermes_->boot_mode;
    default:
        return {};
    }
}

void SettingsScreen::set_field_value(int field, const std::string &value)
{
    switch (field) {
    case FIELD_GATEWAY_URL:
        hermes_->gateway_ws_url = value;
        break;
    case FIELD_GATEWAY_TOKEN:
        hermes_->gateway_token = value;
        break;
    case FIELD_WIFI_SSID:
        wifi_->ssid = value;
        break;
    case FIELD_WIFI_PASS:
        wifi_->password = value;
        break;
    case FIELD_PERSONA:
        hermes_->persona = value.empty() ? DEFAULT_PERSONA : value;
        break;
    case FIELD_BOOT_MODE:
        hermes_->boot_mode = value == "setup" ? "setup" : "console";
        break;
    default:
        break;
    }
}

std::string SettingsScreen::display_value(int field) const
{
    std::string value = field_value(field);
    if (value.empty()) {
        return field == FIELD_GATEWAY_URL ? "(not set — e.g. wss://your-server.example.com/ws/tab5)"
                                          : "(not set)";
    }
    // Secrets: show enough to recognize, never the whole thing.
    if (field == FIELD_GATEWAY_TOKEN || field == FIELD_WIFI_PASS) {
        std::string masked = value.substr(0, std::min<size_t>(4, value.size()));
        masked += "****";
        return masked;
    }
    return value;
}

void SettingsScreen::refresh_row(int field)
{
    lv_label_set_text(row_values_[field], display_value(field).c_str());
    bool selected = field == selected_field_;
    lv_obj_set_style_border_color(rows_[field], lv_color_hex(selected ? COL_SUN : COL_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(rows_[field], selected ? 2 : 1, 0);
}

void SettingsScreen::refresh_edit_line()
{
    if (selected_field_ < 0) {
        lv_label_set_text(edit_label_, "EDIT: (no field selected)");
        return;
    }
    std::string line = "EDIT ";
    line += kFieldNames[selected_field_];
    line += ": ";
    line += edit_buffer_;
    line += "_";
    lv_label_set_text(edit_label_, line.c_str());
}

void SettingsScreen::select_field(int field)
{
    int previous = selected_field_;
    selected_field_ = field;
    edit_buffer_ = field_value(field);
    if (previous >= 0) {
        refresh_row(previous);
    }
    refresh_row(field);
    refresh_edit_line();
}

void SettingsScreen::commit_edit()
{
    if (selected_field_ < 0) {
        return;
    }
    set_field_value(selected_field_, edit_buffer_);
    if (selected_field_ == FIELD_WIFI_SSID || selected_field_ == FIELD_WIFI_PASS) {
        wifi_edited_ = true;
    }
    int field = selected_field_;
    selected_field_ = -1;
    edit_buffer_.clear();
    refresh_row(field);
    refresh_edit_line();
    flash_status("committed - SAVE to persist, SAVE+REBOOT to apply", COL_YELLOW);
}

void SettingsScreen::cancel_edit()
{
    if (selected_field_ < 0) {
        return;
    }
    int field = selected_field_;
    selected_field_ = -1;
    edit_buffer_.clear();
    refresh_row(field);
    refresh_edit_line();
}

void SettingsScreen::cycle_brightness()
{
    constexpr size_t kStepCount = sizeof(kBrightnessSteps) / sizeof(kBrightnessSteps[0]);
    size_t next = 0;
    for (size_t i = 0; i < kStepCount; ++i) {
        if (hermes_->screen_brightness <= kBrightnessSteps[i]) {
            next = (i + 1) % kStepCount;
            break;
        }
    }
    apply_brightness(kBrightnessSteps[next]);
}

void SettingsScreen::cycle_volume()
{
    constexpr size_t kStepCount = sizeof(kVolumeSteps) / sizeof(kVolumeSteps[0]);
    size_t next = 0;
    for (size_t i = 0; i < kStepCount; ++i) {
        if (hermes_->speaker_volume <= kVolumeSteps[i]) {
            next = (i + 1) % kStepCount;
            break;
        }
    }
    hermes_->speaker_volume = kVolumeSteps[next];
    if (volume_apply_ != nullptr) {
        volume_apply_(hermes_->speaker_volume, volume_apply_ctx_);
    }
    char text[16];
    std::snprintf(text, sizeof(text), "%d%%", hermes_->speaker_volume);
    lv_label_set_text(volume_value_, text);
}

void SettingsScreen::apply_brightness(int percent)
{
    percent = std::clamp(percent, 10, 100);
    hermes_->screen_brightness = percent;
    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_display_brightness_set(percent));
    char text[16];
    std::snprintf(text, sizeof(text), "%d%%", percent);
    lv_label_set_text(brightness_value_, text);
}

void SettingsScreen::save_to_nvs()
{
    esp_err_t err = store_->save(*wifi_, *hermes_);
    if (err == ESP_OK) {
        // Wi-Fi edited here outlives SD re-imports; an empty SSID hands
        // control back to the SD profile.
        if (wifi_edited_) {
            store_->save_wifi_override(!wifi_->ssid.empty());
            flash_status(wifi_->ssid.empty()
                             ? "saved - SD profile controls Wi-Fi again"
                             : "saved - these Wi-Fi credentials now beat the SD profile",
                         COL_YELLOW);
        } else {
            flash_status("saved to NVS", COL_YELLOW);
        }
    } else {
        flash_status("NVS save FAILED", COL_ALERT);
        ESP_LOGE(TAG, "settings save failed: %s", esp_err_to_name(err));
    }
}

void SettingsScreen::flash_status(const char *text, uint32_t color)
{
    lv_label_set_text(status_label_, text);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(color), 0);
}

void SettingsScreen::request_close(bool reboot)
{
    cancel_edit();
    active_ = false;
    if (on_close_ != nullptr) {
        on_close_(reboot, close_ctx_);
    }
}

void SettingsScreen::key_input(const uint8_t *data, size_t len)
{
    bool dirty = false;
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        if (byte == '\r' || byte == '\n') {
            commit_edit();
            continue;
        }
        if (byte == 0x1b) {  // ESC: cancel edit, or close when idle
            if (selected_field_ >= 0) {
                cancel_edit();
            } else {
                request_close(false);
                return;
            }
            continue;
        }
        if (selected_field_ < 0) {
            continue;  // not editing: ignore typing
        }
        if (byte == 0x08 || byte == 0x7f) {
            if (!edit_buffer_.empty()) {
                edit_buffer_.pop_back();
                dirty = true;
            }
            continue;
        }
        if (byte >= 0x20 && byte < 0x7f && edit_buffer_.size() < 256) {
            edit_buffer_.push_back(static_cast<char>(byte));
            dirty = true;
        }
    }
    if (dirty) {
        refresh_edit_line();
    }
}

void SettingsScreen::row_event_cb(lv_event_t *e)
{
    auto *self = static_cast<SettingsScreen *>(lv_event_get_user_data(e));
    lv_obj_t *row = static_cast<lv_obj_t *>(lv_event_get_target(e));
    int field = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
    if (field >= 0 && field < FIELD_COUNT) {
        self->select_field(field);
    }
}

void SettingsScreen::button_event_cb(lv_event_t *e)
{
    auto *self = static_cast<SettingsScreen *>(lv_event_get_user_data(e));
    lv_obj_t *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
    auto id = static_cast<SettingsButton>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(btn)));
    switch (id) {
    case BTN_SAVE:
        self->commit_edit();
        self->save_to_nvs();
        break;
    case BTN_REBOOT:
        self->commit_edit();
        self->save_to_nvs();
        self->request_close(true);
        break;
    case BTN_SETUP:
        self->commit_edit();
        self->hermes_->boot_mode = "setup";
        self->save_to_nvs();
        self->request_close(true);
        break;
    case BTN_BACK:
        self->request_close(false);
        break;
    case BTN_BRIGHTNESS:
        self->cycle_brightness();
        break;
    case BTN_VOLUME:
        self->cycle_volume();
        break;
    }
}
