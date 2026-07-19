#include "setup_wizard.hpp"

#include "app_config.hpp"
#include "archie_pointcloud.hpp"
#include "direct_ai_client.hpp"
#include "hermes_theme.hpp"
#include "keyboard_status.hpp"
#include "wifi_manager.hpp"
#include "ws_hermes_client.hpp"

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

using namespace hermes_theme;

namespace {

const char *TAG = "setup_wizard";

enum Step { STEP_WIFI = 0, STEP_MODE, STEP_CONNECTION, STEP_VOICE, STEP_TEST };

// One editable text field on a step.
struct Field {
    const char *label;
    std::string *value;  // points into the profile
    bool secret;
};

struct WizardState {
    ProfileStore *store = nullptr;
    WifiProfile *wifi = nullptr;
    HermesProfile *hermes = nullptr;
    SetupDoneCallback done = nullptr;
    void *ctx = nullptr;

    Step step = STEP_WIFI;
    std::atomic<bool> active{false};

    lv_obj_t *screen = nullptr;
    lv_obj_t *fields_ta[4] = {};  // lv_textarea per field
    lv_obj_t *keyboard = nullptr;
    int field_count = 0;
    int focus = 0;

    // Test step.
    lv_obj_t *check_rows[4] = {};
    lv_obj_t *retry_btn = nullptr;
    lv_obj_t *test_hint = nullptr;
    lv_timer_t *test_poll = nullptr;
    ArchiePointCloud *archie = nullptr;
    std::string voice_toggle = "off";

    Field fields[4];
};

WizardState s;

// ---- shared test results (written by the test task, read by an LVGL timer) -
enum CheckState { CHECK_PENDING = 0, CHECK_PASS, CHECK_FAIL };
struct TestResults {
    std::atomic<CheckState> wifi{CHECK_PENDING};
    std::atomic<CheckState> gateway{CHECK_PENDING};
    std::atomic<CheckState> token{CHECK_PENDING};
    std::atomic<CheckState> hermes{CHECK_PENDING};
    std::atomic<bool> finished{false};
};
TestResults s_test;
WsHermesClient s_test_ws;
DirectAiClient s_test_direct;
std::atomic<bool> s_ws_linked{false};
std::atomic<bool> s_ws_status_seen{false};
std::atomic<bool> s_direct_done{false};
std::atomic<bool> s_direct_ok{false};

void test_on_link(bool connected, void *) { s_ws_linked = connected; }
void test_on_status(const char *, const char *, uint32_t, uint32_t, void *) { s_ws_status_seen = true; }
void test_on_log(const char *, void *) { s_ws_status_seen = true; }
void test_direct_chat(const char *, const char *text, bool done, void *)
{
    if (done) {
        s_direct_ok = text != nullptr && text[0] != '\0';
        s_direct_done = true;
    }
}
void test_direct_status(const char *, const char *state, uint32_t, uint32_t, void *)
{
    if (state != nullptr && std::strcmp(state, "error") == 0) {
        s_direct_ok = false;
        s_direct_done = true;
    }
}
void test_direct_link(bool, void *) {}
void test_direct_log(const char *, void *) {}

void enter_wifi_step();
void enter_mode_step();
void enter_connection_step();
void enter_voice_step();
void enter_test_step();
void advance_step();
void go_back();

// ---- shared chrome ---------------------------------------------------------

// Five-stage trail across the top; the active
// step glows amber, completed steps sun-orange, future steps dim.
void add_progress_trail(lv_obj_t *parent, int active_step)
{
    static const char *kNames[5] = {"1 WIFI", "2 LINK", "3 CREDENTIALS", "4 VOICE", "5 TEST"};
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 980, 30);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, kNames[i]);
        uint32_t color = i == active_step ? COL_AMBER : i < active_step ? COL_SUN : COL_DIM;
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
        lv_obj_set_style_text_font(label, font_mono(), 0);
        if (i < 4) {
            lv_obj_t *dash = lv_label_create(row);
            lv_label_set_text(dash, "──");
            lv_obj_set_style_text_color(dash, lv_color_hex(COL_PANEL_EDGE), 0);
            lv_obj_set_style_text_font(dash, font_mono(), 0);
        }
    }
}

void nav_button_cb(lv_event_t *e)
{
    intptr_t id = reinterpret_cast<intptr_t>(lv_obj_get_user_data(
        static_cast<lv_obj_t *>(lv_event_get_target(e))));
    if (id == 1) {
        advance_step();
    } else {
        go_back();
    }
}

// BACK (unless first step) and NEXT touch buttons flanking the title row.
void add_nav_buttons(lv_obj_t *parent, bool show_back, bool show_next)
{
    if (show_back) {
        lv_obj_t *back = make_deck_button(parent, "< BACK", COL_DIM);
        lv_obj_set_size(back, 150, 44);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, 24, 30);
        lv_obj_set_user_data(back, reinterpret_cast<void *>(static_cast<intptr_t>(0)));
        lv_obj_add_event_cb(back, nav_button_cb, LV_EVENT_CLICKED, nullptr);
    }
    if (show_next) {
        lv_obj_t *next = make_deck_button(parent, "NEXT >", COL_AMBER);
        lv_obj_set_size(next, 150, 44);
        lv_obj_align(next, LV_ALIGN_TOP_RIGHT, -24, 30);
        lv_obj_set_user_data(next, reinterpret_cast<void *>(static_cast<intptr_t>(1)));
        lv_obj_add_event_cb(next, nav_button_cb, LV_EVENT_CLICKED, nullptr);
    }
}

// ---- field steps (textareas + on-screen keyboard) ---------------------------

constexpr int kKbH = 260;

// Standard LVGL pattern (see docs "Keyboard" example): the on-screen keyboard
// follows textarea focus — FOCUSED/CLICKED shows it, DEFOCUSED hides it — with
// two device-specific twists: a slide animation instead of a hard pop, and
// "hardware keys win": any physical keystroke tucks it away, and it starts
// hidden when the snap-on keyboard is already attached.
void kb_slide_cb(void *var, int32_t v)
{
    lv_obj_set_y(static_cast<lv_obj_t *>(var), v);
}

void kb_hide_done_cb(lv_anim_t *a)
{
    lv_obj_add_flag(static_cast<lv_obj_t *>(a->var), LV_OBJ_FLAG_HIDDEN);
}

void set_keyboard_visible(bool visible)
{
    if (s.keyboard == nullptr) {
        return;
    }
    lv_anim_delete(s.keyboard, kb_slide_cb);
    int32_t from = lv_obj_get_y(s.keyboard);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s.keyboard);
    lv_anim_set_exec_cb(&a, kb_slide_cb);
    if (visible) {
        lv_obj_remove_flag(s.keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_anim_set_values(&a, from, LCD_H - kKbH);
        lv_anim_set_duration(&a, 220);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    } else {
        lv_anim_set_values(&a, from, LCD_H);
        lv_anim_set_duration(&a, 180);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_set_completed_cb(&a, kb_hide_done_cb);
    }
    lv_anim_start(&a);
}

void apply_focus_styles()
{
    for (int i = 0; i < s.field_count; ++i) {
        bool focused = i == s.focus;
        // Color/opacity only — changing border WIDTH on focus nudges the
        // textarea content by a pixel ("jumping text").
        lv_obj_set_style_border_color(s.fields_ta[i],
                                      lv_color_hex(focused ? COL_SUN : COL_PANEL_EDGE), 0);
        lv_obj_set_style_border_opa(s.fields_ta[i], focused ? LV_OPA_COVER : LV_OPA_60, 0);
    }
    if (s.keyboard != nullptr) {
        lv_keyboard_set_textarea(s.keyboard, s.fields_ta[s.focus]);
    }
}

void focus_field(int index)
{
    if (index < 0 || index >= s.field_count) {
        return;
    }
    s.focus = index;
    apply_focus_styles();
}

// Persist every textarea back into its profile string.
void store_fields()
{
    for (int i = 0; i < s.field_count; ++i) {
        *s.fields[i].value = lv_textarea_get_text(s.fields_ta[i]);
    }
}

void textarea_event_cb(lv_event_t *e)
{
    auto *ta = static_cast<lv_obj_t *>(lv_event_get_target(e));
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DEFOCUSED) {
        // Focus left the fields (e.g. NEXT/BACK tapped): keyboard slides away.
        // If another field takes focus right after, its FOCUSED event cancels
        // the slide and brings it back.
        set_keyboard_visible(false);
        return;
    }
    for (int i = 0; i < s.field_count; ++i) {
        if (s.fields_ta[i] == ta) {
            focus_field(i);
            // Focusing/tapping a field always brings the keyboard back.
            set_keyboard_visible(true);
            return;
        }
    }
}

void keyboard_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) {
        // ✓ on the on-screen keyboard: next field, or advance on the last.
        if (s.focus + 1 < s.field_count) {
            focus_field(s.focus + 1);
        } else {
            advance_step();
        }
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_CANCEL) {
        // Keyboard hide key: tuck it away; tapping any field re-opens it.
        set_keyboard_visible(false);
    }
}

void build_field_step(const char *title, const char *hint, int step_index)
{
    s.screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s.screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_pad_all(s.screen, 0, 0);
    lv_obj_clear_flag(s.screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(s.screen);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_text_font(title_label, font_title(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 30);

    add_progress_trail(s.screen, step_index);
    add_nav_buttons(s.screen, step_index > 0, true);

    // Fields panel sits between the trail and the keyboard.
    const int panel_x = 200;
    const int panel_w = LCD_W - 2 * panel_x;
    const int panel_y = 116;
    const int panel_h = LCD_H - kKbH - panel_y - 46;
    lv_obj_t *panel = make_panel(s.screen, "SETUP", panel_x, panel_y, panel_w, panel_h);
    add_corner_brackets(panel);

    const int row_h = s.field_count > 2 ? 62 : 78;
    const int row_gap = s.field_count > 2 ? 8 : 16;
    for (int i = 0; i < s.field_count; ++i) {
        lv_obj_t *name = lv_label_create(panel);
        lv_label_set_text(name, s.fields[i].label);
        lv_obj_set_style_text_color(name, lv_color_hex(COL_SUN), 0);
        lv_obj_set_style_text_font(name, font_mono(), 0);
        lv_obj_set_pos(name, 0, i * (row_h + row_gap));

        lv_obj_t *ta = lv_textarea_create(panel);
        s.fields_ta[i] = ta;
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_max_length(ta, 255);
        lv_textarea_set_text(ta, s.fields[i].value->c_str());
        if (s.fields[i].secret) {
            lv_textarea_set_password_mode(ta, true);
        }
        lv_obj_set_size(ta, panel_w - 40, row_h - 26);
        lv_obj_set_pos(ta, 0, i * (row_h + row_gap) + 24);
        lv_obj_set_style_bg_color(ta, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_text_font(ta, font_mono(), 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(COL_PANEL_EDGE), 0);
        lv_obj_set_style_border_width(ta, 2, 0);
        lv_obj_set_style_radius(ta, 4, 0);
        lv_obj_add_event_cb(ta, textarea_event_cb, LV_EVENT_FOCUSED, nullptr);
        lv_obj_add_event_cb(ta, textarea_event_cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(ta, textarea_event_cb, LV_EVENT_DEFOCUSED, nullptr);
    }

    lv_obj_t *hint_label = lv_label_create(s.screen);
    lv_label_set_text(hint_label, hint);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(hint_label, font_mono(), 0);
    lv_obj_align(hint_label, LV_ALIGN_TOP_MID, 0, panel_y + panel_h + 10);

    // Snap-on keyboard indicator (refreshed each step build): reassures
    // hardware-keyboard users that TAB/ENTER will work.
    lv_obj_t *kbd = lv_label_create(s.screen);
    lv_label_set_text(kbd, keyboard_status_get() ? "KB: hardware detected" : "KB: on-screen");
    lv_obj_set_style_text_color(kbd, lv_color_hex(keyboard_status_get() ? COL_YELLOW : COL_DIM), 0);
    lv_obj_set_style_text_font(kbd, font_mono(), 0);
    lv_obj_align(kbd, LV_ALIGN_TOP_LEFT, 24, panel_y + panel_h + 10);

    // On-screen keyboard docked at the bottom: no hardware keyboard required.
    // When the snap-on keyboard is attached it starts tucked away (slide up
    // on field tap); touch-only users get it up front.
    s.keyboard = lv_keyboard_create(s.screen);
    lv_obj_set_size(s.keyboard, LCD_W, kKbH);
    bool hw_kbd = keyboard_status_get();
    lv_obj_set_pos(s.keyboard, 0, hw_kbd ? LCD_H : LCD_H - kKbH);
    if (hw_kbd) {
        lv_obj_add_flag(s.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_bg_color(s.keyboard, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_color(s.keyboard, lv_color_hex(COL_BG), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s.keyboard, lv_color_hex(COL_TEXT), LV_PART_ITEMS);
    lv_obj_add_event_cb(s.keyboard, keyboard_event_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(s.keyboard, keyboard_event_cb, LV_EVENT_CANCEL, nullptr);

    // A full-screen cross-fade forces both 1280x720 trees through alpha
    // compositing. Swap atomically and let LVGL delete the previous tree.
    lv_screen_load_anim(s.screen, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);
    focus_field(0);
}

// ---- step transitions ------------------------------------------------------

void enter_wifi_step()
{
    s.step = STEP_WIFI;
    s.field_count = 2;
    s.fields[0] = {"WIFI SSID", &s.wifi->ssid, false};
    s.fields[1] = {"WIFI PASSWORD", &s.wifi->password, true};
    build_field_step("ARCHIE // NEXUS SETUP", "tap a field · NEXT (or ENTER on the last field) to continue",
                     0);
}

void enter_mode_step()
{
    s.step = STEP_MODE;
    s.field_count = 2;
    s.fields[0] = {"LINK TYPE (hermes/openclaw/openai/claude/custom)",
                   &s.hermes->connection_mode, false};
    s.fields[1] = {"GUARDIAN PERSONA", &s.hermes->persona, false};
    build_field_step("CHOOSE THE NEXUS",
                     "gateways keep keys off-device · direct providers use HTTPS from this Tab5", 1);
}

void apply_mode_defaults()
{
    if (s.hermes->connection_mode == "openclaw") {
        if (s.hermes->api_base_url.empty() ||
            s.hermes->api_base_url == "https://api.openai.com/v1" ||
            s.hermes->api_base_url == "https://api.anthropic.com") {
            s.hermes->api_base_url = "http://openclaw-host.local:18789/v1";
        }
        if (s.hermes->api_model.empty() || s.hermes->api_model == "gpt-5.6-terra" ||
            s.hermes->api_model == "claude-sonnet-5") {
            s.hermes->api_model = "openclaw/default";
        }
    } else if (s.hermes->connection_mode == "claude") {
        if (s.hermes->api_base_url.empty() ||
            s.hermes->api_base_url == "https://api.openai.com/v1" ||
            s.hermes->api_base_url == "http://openclaw-host.local:18789/v1") {
            s.hermes->api_base_url = "https://api.anthropic.com";
        }
        if (s.hermes->api_model.empty() || s.hermes->api_model == "gpt-5.6-terra") {
            s.hermes->api_model = "claude-sonnet-5";
        }
    } else if (s.hermes->connection_mode == "openai") {
        if (s.hermes->api_base_url.empty() ||
            s.hermes->api_base_url == "https://api.anthropic.com" ||
            s.hermes->api_base_url == "http://openclaw-host.local:18789/v1") {
            s.hermes->api_base_url = "https://api.openai.com/v1";
        }
        if (s.hermes->api_model.empty() || s.hermes->api_model == "claude-sonnet-5" ||
            s.hermes->api_model == "openclaw/default") {
            s.hermes->api_model = "gpt-5.6-terra";
        }
    }
}

void enter_connection_step()
{
    s.step = STEP_CONNECTION;
    bool gateway = s.hermes->connection_mode == "hermes";
    if (gateway) {
        s.field_count = 2;
        s.fields[0] = {"GATEWAY ADAPTER HOST OR URL", &s.hermes->gateway_ws_url, false};
        s.fields[1] = {"GATEWAY TOKEN", &s.hermes->gateway_token, true};
        build_field_step("ANCHOR THE GATEWAY",
                         "local host/IP -> ws://HOST:8787 · public host -> wss://HOST · token required", 2);
        return;
    }
    s.field_count = 3;
    s.fields[0] = {"HTTPS API BASE URL", &s.hermes->api_base_url, false};
    s.fields[1] = {"PROVIDER API KEY", &s.hermes->api_key, true};
    s.fields[2] = {"MODEL", &s.hermes->api_model, false};
    build_field_step(
        s.hermes->connection_mode == "openclaw" ? "ANCHOR OPENCLAW" : "ANCHOR THE PROVIDER",
        s.hermes->connection_mode == "openclaw"
            ? "enable chatCompletions · keep the operator bearer token on private LAN/tailnet"
            : "the key is stored in device NVS only · use a scoped, low-limit key",
        2);
}

void enter_voice_step()
{
    s.step = STEP_VOICE;
    s.voice_toggle = s.hermes->voice_enabled ? "on" : "off";
    s.field_count = 3;
    s.fields[0] = {"VOICE (on/off)", &s.voice_toggle, false};
    s.fields[1] = {"ELEVENLABS API KEY (optional)", &s.hermes->elevenlabs_key, true};
    s.fields[2] = {"ELEVENLABS VOICE ID (optional)", &s.hermes->elevenlabs_voice_id, false};
    build_field_step("VOICE OF THE GUARDIAN",
                     "leave off to skip · gateway mode keeps production voice keys on the server", 3);
}

void set_check_row(int idx, const char *text, CheckState state)
{
    uint32_t color = state == CHECK_PASS  ? COL_YELLOW
                     : state == CHECK_FAIL ? COL_ALERT
                                           : COL_DIM;
    const char *mark = state == CHECK_PASS ? "PASS" : state == CHECK_FAIL ? "FAIL" : "····";
    char line[64];
    std::snprintf(line, sizeof(line), "%-10s %s", text, mark);
    lv_label_set_text(s.check_rows[idx], line);
    lv_obj_set_style_text_color(s.check_rows[idx], lv_color_hex(color), 0);
}

// The test runs off the LVGL task so the blocking Wi-Fi connect doesn't stall
// the UI; this task posts results into s_test, an LVGL timer paints them.
void test_task(void *)
{
    // 1. Wi-Fi.
    esp_err_t werr = wifi_connect_sta(s.wifi->ssid.c_str(), s.wifi->password.c_str(), 50000);
    if (werr != ESP_OK) {
        s_test.wifi = CHECK_FAIL;
        s_test.gateway = CHECK_FAIL;
        s_test.token = CHECK_FAIL;
        s_test.hermes = CHECK_FAIL;
        s_test.finished = true;
        vTaskDelete(nullptr);
        return;
    }
    s_test.wifi = CHECK_PASS;

    // TLS needs a sane clock (LetsEncrypt certs read "not yet valid" in
    // 1970). SNTP was started at boot; give it a moment now that Wi-Fi is up.
    const bool gateway_mode = s.hermes->connection_mode == "hermes";
    const bool tls_link = gateway_mode
                              ? s.hermes->gateway_ws_url.rfind("wss://", 0) == 0
                              : s.hermes->api_base_url.rfind("https://", 0) == 0;
    if (tls_link) {
        for (int i = 0; i < 80; ++i) {  // up to 8s
            std::time_t now = std::time(nullptr);
            std::tm tm_now = {};
            localtime_r(&now, &tm_now);
            if (tm_now.tm_year + 1900 >= 2025) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    if (!gateway_mode) {
        // One small live prompt validates DNS/TLS, the key, model access and
        // response parsing. Direct mode intentionally has no hidden proxy.
        s_direct_done = false;
        s_direct_ok = false;
        DirectAiCallbacks cb = {};
        cb.on_chat_text = test_direct_chat;
        cb.on_status = test_direct_status;
        cb.on_link = test_direct_link;
        cb.on_log_line = test_direct_log;
        DirectAiConfig cfg;
        cfg.provider = s.hermes->connection_mode;
        cfg.base_url = s.hermes->api_base_url;
        cfg.api_key = s.hermes->api_key;
        cfg.model = s.hermes->api_model;
        s_test_direct.stop();
        if (s_test_direct.begin(cfg, cb) != ESP_OK ||
            s_test_direct.send_chat("archie", "Reply with exactly: NEXUS ONLINE") != ESP_OK) {
            s_test.gateway = CHECK_FAIL;
            s_test.token = CHECK_FAIL;
            s_test.hermes = CHECK_FAIL;
            s_test.finished = true;
            vTaskDelete(nullptr);
            return;
        }
        s_test.gateway = CHECK_PASS;  // HTTPS client initialized.
        for (int i = 0; i < 600 && !s_direct_done.load(); ++i) {  // up to 60s
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        const bool provider_ok = s_direct_done.load() && s_direct_ok.load();
        s_test.token = provider_ok ? CHECK_PASS : CHECK_FAIL;
        s_test.hermes = provider_ok ? CHECK_PASS : CHECK_FAIL;
        s_test_direct.stop();
        s_test.finished = true;
        vTaskDelete(nullptr);
        return;
    }

    // Gateway mode: adapter + token + first status.
    s_ws_linked = false;
    s_ws_status_seen = false;
    WsHermesCallbacks cb = {};
    cb.on_chat_text = nullptr;
    cb.on_status = test_on_status;
    cb.on_vision_result = nullptr;
    cb.on_tts_ready = nullptr;
    cb.on_link = test_on_link;
    cb.on_log_line = test_on_log;
    cb.ctx = nullptr;
    WsHermesConfig cfg;
    cfg.uri = s.hermes->gateway_ws_url;
    cfg.token = s.hermes->gateway_token;
    cfg.backend = s.hermes->connection_mode;

    // A prior RETRY may have left a client running; begin() would orphan it
    // into an endless background reconnect loop.
    s_test_ws.stop();
    if (s_test_ws.begin(cfg, cb) != ESP_OK) {
        s_test.gateway = CHECK_FAIL;
        s_test.token = CHECK_FAIL;
        s_test.hermes = CHECK_FAIL;
        s_test.finished = true;
        vTaskDelete(nullptr);
        return;
    }

    // Wait for the socket to come up (gateway PASS).
    bool linked = false;
    for (int i = 0; i < 100; ++i) {  // up to 10s
        if (s_ws_linked.load()) {
            linked = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_test.gateway = linked ? CHECK_PASS : CHECK_FAIL;
    if (!linked) {
        s_test.token = CHECK_FAIL;
        s_test.hermes = CHECK_FAIL;
        s_test_ws.stop();
        s_test.finished = true;
        vTaskDelete(nullptr);
        return;
    }

    // The gateway closes the socket on a bad token; a status/log envelope (or
    // a link that stays up past the grace window) means the token was accepted.
    bool accepted = false;
    for (int i = 0; i < 60; ++i) {  // up to 6s
        if (s_ws_status_seen.load()) {
            accepted = true;
            break;
        }
        if (!s_ws_linked.load()) {  // closed -> rejected
            accepted = false;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!accepted && s_ws_linked.load()) {
        accepted = true;
    }
    s_test.token = accepted ? CHECK_PASS : CHECK_FAIL;
    // Distinct from the token check: ONLINE means the agent actually SPOKE
    // (a status/log envelope arrived), not merely that the socket survived.
    s_test.hermes = (accepted && s_ws_status_seen.load()) ? CHECK_PASS : CHECK_FAIL;

    s_test_ws.stop();
    s_test.finished = true;
    vTaskDelete(nullptr);
}

void retry_button_cb(lv_event_t *)
{
    enter_test_step();
}

void test_poll_cb(lv_timer_t *)
{
    const bool gateway_mode = s.hermes->connection_mode == "hermes";
    set_check_row(0, "WiFi", s_test.wifi.load());
    set_check_row(1, gateway_mode ? "Gateway" : "HTTPS", s_test.gateway.load());
    set_check_row(2, gateway_mode ? "Token" : "API key", s_test.token.load());
    set_check_row(3, gateway_mode ? "Agent" : "Model", s_test.hermes.load());

    if (!s_test.finished.load()) {
        return;
    }
    // Done: stop polling.
    if (s.test_poll != nullptr) {
        lv_timer_delete(s.test_poll);
        s.test_poll = nullptr;
    }
    bool all_pass = s_test.wifi == CHECK_PASS && s_test.gateway == CHECK_PASS &&
                    s_test.token == CHECK_PASS && s_test.hermes == CHECK_PASS;
    if (all_pass) {
        archie_pointcloud_set_state(s.archie, ArchieVisualState::Online);
        archie_pointcloud_pulse(s.archie);
        // Persist and hand control to the console.
        s.hermes->boot_mode = "console";
        s.store->save(*s.wifi, *s.hermes);
        s.store->save_wifi_override(!s.wifi->ssid.empty());
        s.active = false;
        // The console replaces this screen with an auto-delete swap. Stop the
        // timer now and release its PSRAM when LVGL deletes the old canvas.
        archie_pointcloud_forget(s.archie);
        s.archie = nullptr;
        if (s.done != nullptr) {
            s.done(true, s.ctx);
        }
    } else {
        archie_pointcloud_set_state(s.archie, ArchieVisualState::Error);
        if (s.retry_btn != nullptr) {
            lv_obj_remove_flag(s.retry_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (s.test_hint != nullptr) {
            const char *why;
            if (s_test.wifi == CHECK_FAIL) {
                why = "Wi-Fi failed · wrong password, or the AP has no 2.4GHz band";
            } else if (s_test.gateway == CHECK_FAIL) {
                why = gateway_mode
                          ? "gateway unreachable · is the adapter running and the URL correct?"
                          : "HTTPS unreachable · verify the API base URL and network";
            } else if (s_test.token == CHECK_FAIL) {
                why = gateway_mode
                          ? "token rejected · it must match the adapter token"
                          : "API key rejected · check scope, balance and model access";
            } else {
                why = gateway_mode
                          ? "no reply from the agent · check the adapter logs"
                          : "provider returned no model text · verify the model ID";
            }
            lv_label_set_text(s.test_hint, why);
            lv_obj_set_style_text_color(s.test_hint, lv_color_hex(COL_ALERT), 0);
        }
    }
}

void enter_test_step()
{
    s.step = STEP_TEST;

    // RETRY replaces an existing test screen; disarm its canvas before the
    // auto-delete transition begins.
    if (s.archie != nullptr) {
        archie_pointcloud_forget(s.archie);
        s.archie = nullptr;
    }

    // Persist what we have so a reboot keeps it even if the test fails.
    s.store->save(*s.wifi, *s.hermes);

    s.screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s.screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_pad_all(s.screen, 0, 0);
    lv_obj_clear_flag(s.screen, LV_OBJ_FLAG_SCROLLABLE);
    s.keyboard = nullptr;

    lv_obj_t *title = lv_label_create(s.screen);
    lv_label_set_text(title, "CONNECTION TEST");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_AMBER), 0);
    lv_obj_set_style_text_font(title, font_title(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    add_progress_trail(s.screen, 4);
    add_nav_buttons(s.screen, true, false);

    s.archie = archie_pointcloud_create(s.screen, 320, 280, COL_BG, false);
    if (s.archie != nullptr) {
        lv_obj_align(s.archie->canvas, LV_ALIGN_TOP_MID, 0, 108);
        archie_pointcloud_set_state(s.archie, ArchieVisualState::Linking);
    }

    lv_obj_t *panel = make_panel(s.screen, "LINK CHECK", 420, 310, 440, 200);
    add_corner_brackets(panel);
    for (int i = 0; i < 4; ++i) {
        s.check_rows[i] = lv_label_create(panel);
        lv_obj_set_style_text_font(s.check_rows[i], font_mono(), 0);
        lv_obj_set_pos(s.check_rows[i], 8, 8 + i * 36);
    }
    set_check_row(0, "WiFi", CHECK_PENDING);
    const bool gateway_mode = s.hermes->connection_mode == "hermes";
    set_check_row(1, gateway_mode ? "Gateway" : "HTTPS", CHECK_PENDING);
    set_check_row(2, gateway_mode ? "Token" : "API key", CHECK_PENDING);
    set_check_row(3, gateway_mode ? "Agent" : "Model", CHECK_PENDING);

    // Retry appears (hidden for now) once a test run finishes with a FAIL.
    s.retry_btn = make_deck_button(s.screen, "↻ RETRY", COL_YELLOW);
    lv_obj_set_size(s.retry_btn, 200, 48);
    lv_obj_align(s.retry_btn, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_add_event_cb(s.retry_btn, retry_button_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s.retry_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hint = lv_label_create(s.screen);
    s.test_hint = hint;
    lv_label_set_text(hint, "testing… BACK to edit");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(hint, font_mono(), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    lv_screen_load_anim(s.screen, LV_SCREEN_LOAD_ANIM_NONE, 0, 0, true);

    // Reset shared results and launch the worker + poller.
    s_test.wifi = CHECK_PENDING;
    s_test.gateway = CHECK_PENDING;
    s_test.token = CHECK_PENDING;
    s_test.hermes = CHECK_PENDING;
    s_test.finished = false;
    s.test_poll = lv_timer_create(test_poll_cb, 150, nullptr);
    if (xTaskCreate(test_task, "wizard_test", 6144, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "failed to start test task");
        s_test.wifi = CHECK_FAIL;
        s_test.finished = true;
    }
}

// Let users type just a host. Local names/private IPv4 addresses target the
// included LAN adapter (ws:// on :8787); public DNS names default to wss://.
// Explicit schemes, ports, and paths are left untouched.
std::string normalize_ws_url(std::string url)
{
    size_t a = url.find_first_not_of(" \t");
    size_t b = url.find_last_not_of(" \t");
    if (a == std::string::npos) {
        return "";
    }
    url = url.substr(a, b - a + 1);

    bool has_scheme = url.rfind("ws://", 0) == 0 || url.rfind("wss://", 0) == 0;
    if (!has_scheme) {
        size_t slash = url.find('/');
        std::string authority = slash == std::string::npos ? url : url.substr(0, slash);
        std::string host = authority;
        bool has_port = false;
        if (!host.empty() && host.front() == '[') {
            const size_t bracket = host.find(']');
            if (bracket != std::string::npos) {
                has_port = bracket + 1 < host.size() && host[bracket + 1] == ':';
                host = host.substr(1, bracket - 1);
            }
        } else {
            const size_t colon = host.rfind(':');
            // A single colon denotes host:port. Multiple colons denote an
            // unbracketed IPv6 literal, where a port would be ambiguous.
            has_port = colon != std::string::npos && host.find(':') == colon;
            if (has_port) {
                host.resize(colon);
            }
        }
        const bool local_name = host == "localhost" ||
                                (host.find('.') == std::string::npos && host.find(':') == std::string::npos) ||
                                (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0);
        int a = -1;
        int b = -1;
        int c = -1;
        int d = -1;
        char tail = '\0';
        const bool valid_ipv4 = std::sscanf(host.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) == 4 &&
                                a >= 0 && a <= 255 && b >= 0 && b <= 255 &&
                                c >= 0 && c <= 255 && d >= 0 && d <= 255;
        const bool private_ipv4 = valid_ipv4 &&
                                  (a == 10 || a == 127 || (a == 169 && b == 254) ||
                                   (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168));
        const bool local_ipv6 = host == "::1" || host.rfind("fe80:", 0) == 0 ||
                                host.rfind("FE80:", 0) == 0 || host.rfind("fc", 0) == 0 ||
                                host.rfind("FC", 0) == 0 || host.rfind("fd", 0) == 0 ||
                                host.rfind("FD", 0) == 0;
        const bool local = local_name || private_ipv4 || local_ipv6;
        url = std::string(local ? "ws://" : "wss://") + url;
        if (local && !has_port) {
            size_t insert_at = url.find('/', 5);
            url.insert(insert_at == std::string::npos ? url.size() : insert_at, ":8787");
        }
    }
    size_t scheme_end = url.find("://");
    size_t path_slash = url.find('/', scheme_end + 3);
    if (path_slash == std::string::npos) {
        url += "/ws/tab5";
    }
    return url;
}

void advance_step()
{
    if (s.test_poll != nullptr) {
        lv_timer_delete(s.test_poll);
        s.test_poll = nullptr;
    }
    if (s.step == STEP_WIFI) {
        store_fields();
        enter_mode_step();
    } else if (s.step == STEP_MODE) {
        store_fields();
        if (s.hermes->connection_mode != "openclaw" &&
            s.hermes->connection_mode != "openai" &&
            s.hermes->connection_mode != "claude" &&
            s.hermes->connection_mode != "custom") {
            s.hermes->connection_mode = "hermes";
        }
        if (s.hermes->persona.empty()) {
            s.hermes->persona = DEFAULT_PERSONA;
        }
        apply_mode_defaults();
        enter_connection_step();
    } else if (s.step == STEP_CONNECTION) {
        store_fields();
        if (s.hermes->connection_mode == "hermes") {
            s.hermes->gateway_ws_url = normalize_ws_url(s.hermes->gateway_ws_url);
        }
        enter_voice_step();
    } else if (s.step == STEP_VOICE) {
        store_fields();
        s.hermes->voice_enabled = s.voice_toggle == "on" || s.voice_toggle == "yes" ||
                                  s.voice_toggle == "1" || s.voice_toggle == "true";
        enter_test_step();
    }
}

void go_back()
{
    if (s.test_poll != nullptr) {
        lv_timer_delete(s.test_poll);
        s.test_poll = nullptr;
    }
    s_test_ws.stop();
    s_test_direct.stop();
    if (s.archie != nullptr) {
        archie_pointcloud_forget(s.archie);
        s.archie = nullptr;
    }
    if (s.step == STEP_MODE) {
        store_fields();
        enter_wifi_step();
    } else if (s.step == STEP_CONNECTION) {
        store_fields();
        enter_mode_step();
    } else if (s.step == STEP_VOICE) {
        store_fields();
        enter_connection_step();
    } else if (s.step == STEP_TEST) {
        enter_voice_step();
    }
}

}  // namespace

void setup_wizard_start(ProfileStore *store, WifiProfile *wifi, HermesProfile *hermes,
                        SetupDoneCallback done, void *ctx)
{
    s.store = store;
    s.wifi = wifi;
    s.hermes = hermes;
    s.done = done;
    s.ctx = ctx;
    s.active = true;
    hermes_theme::init();

    // Don't ask what the device already knows: when Wi-Fi is already up
    // (e.g. provisioned over USB via Improv), start on link selection —
    // BACK still reaches the Wi-Fi fields.
    wifi_ap_record_t ap = {};
    if (!wifi->ssid.empty() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi already connected; starting wizard at link selection");
        enter_mode_step();
        return;
    }
    enter_wifi_step();
}

bool setup_wizard_active()
{
    return s.active.load();
}

// Physical keyboard (if attached) mirrors the touch flow: printables type into
// the focused field, TAB cycles, ENTER advances, ESC goes back.
void setup_wizard_key_input(const uint8_t *data, size_t len)
{
    if (s.step == STEP_TEST) {
        for (size_t i = 0; i < len; ++i) {
            if (data[i] == 0x1b) {
                go_back();
                return;
            }
        }
        return;
    }

    // Hardware keys in use -> the on-screen keyboard is dead weight; slide it
    // away. Tapping any field brings it back.
    if (len > 0 && s.keyboard != nullptr && !lv_obj_has_flag(s.keyboard, LV_OBJ_FLAG_HIDDEN)) {
        set_keyboard_visible(false);
    }

    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        if (byte == '\t') {
            focus_field((s.focus + 1) % s.field_count);
            continue;
        }
        if (byte == '\r' || byte == '\n') {
            if (s.focus + 1 < s.field_count) {
                focus_field(s.focus + 1);
            } else {
                advance_step();
                return;
            }
            continue;
        }
        if (byte == 0x1b) {
            go_back();
            return;
        }
        lv_obj_t *ta = s.fields_ta[s.focus];
        if (ta == nullptr) {
            continue;
        }
        if (byte == 0x08 || byte == 0x7f) {
            lv_textarea_delete_char(ta);
            continue;
        }
        if (byte >= 0x20 && byte < 0x7f) {
            lv_textarea_add_char(ta, byte);
        }
    }
}
