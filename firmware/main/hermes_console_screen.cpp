#include "hermes_console_screen.hpp"

#include "app_config.hpp"
#include "hermes_theme.hpp"
#include "keyboard_status.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace hermes_theme;

// Two-column Archie Command Centre geometry (1280x720).
namespace {
constexpr int kPad = 8;
constexpr int kStatusH = 44;
constexpr int kBodyY = kStatusH + 14;
constexpr int kScanTrackW = 240;
constexpr int kScanTrackH = 12;
constexpr int kScanPuckW = 36;
constexpr int kRailX = kPad;
constexpr int kRailW = 348;
constexpr int kLogX = kRailX + kRailW + kPad;
constexpr int kLogW = LCD_W - kLogX - kPad;
constexpr int kDeckH = 64;
constexpr int kDeckY = LCD_H - kDeckH - kPad;
constexpr int kInputH = 34;
constexpr int kMaxLogEntries = 120;

// Action tile ids carried in event user data.
enum ActionId : intptr_t {
    ACT_BRIEF = 1,
    ACT_TASKS,
    ACT_PERSONA,
    ACT_SETTINGS,
    ACT_STOP,
    ACT_CLEAR,
};

void scan_anim_cb(void *object, int32_t x)
{
    lv_obj_set_x(static_cast<lv_obj_t *>(object), x);
}

// Gateway/LLM text arrives as arbitrary UTF-8 (smart quotes, emoji, ellipses)
// but the proportional fonts we ship are ASCII-only — unknown glyphs render
// as boxes. Map the common typography to ASCII and drop the rest.
std::string ascii_sanitize(const char *text)
{
    std::string out;
    if (text == nullptr) {
        return out;
    }
    const auto *s = reinterpret_cast<const unsigned char *>(text);
    while (*s != 0) {
        unsigned char c = *s;
        if (c < 0x80) {
            if (c == '\t') {
                out += ' ';
            } else if (c >= 0x20 || c == '\n') {
                out += static_cast<char>(c);
            }
            ++s;
            continue;
        }
        // Decode one UTF-8 code point.
        uint32_t cp = 0;
        int extra = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { ++s; continue; }  // stray continuation byte
        ++s;
        for (int i = 0; i < extra && (*s & 0xC0) == 0x80; ++i, ++s) {
            cp = (cp << 6) | (*s & 0x3F);
        }
        switch (cp) {
        case 0x2018: case 0x2019: case 0x02BC: out += '\''; break;
        case 0x201C: case 0x201D: out += '"'; break;
        case 0x2013: case 0x2014: case 0x2015: out += '-'; break;
        case 0x2026: out += "..."; break;
        case 0x00A0: out += ' '; break;
        case 0x2022: case 0x00B7: out += '-'; break;
        default:
            break;  // emoji / other symbols: drop rather than box
        }
    }
    return out;
}

}  // namespace

void HermesConsoleScreen::create(lv_indev_t *, const ConsoleActions &actions)
{
    actions_ = actions;
    hermes_theme::init();

    screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_clear_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    build_header(screen_);
    build_hero(screen_);
    build_overview(screen_);
    build_chat(screen_);
    build_actions(screen_);

    append_log("NVS AGENT OS 0.1.0 - nexus core online", COL_SUN);
    append_log("-------- SESSION START --------", COL_DIM);
    apply_persona_visuals();
}

// ---- header -----------------------------------------------------------------

void HermesConsoleScreen::build_header(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, LCD_W, kStatusH);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(COL_YELLOW), 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_hor(bar, 12, 0);
    lv_obj_set_style_pad_ver(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ident = lv_label_create(bar);
    lv_label_set_text(ident, "ARCHIE // COMMAND CENTRE");
    lv_obj_set_style_text_color(ident, lv_color_hex(COL_YELLOW), 0);
    lv_obj_set_style_text_font(ident, font_mono(), 0);
    lv_obj_align(ident, LV_ALIGN_LEFT_MID, 0, 0);

    scan_track_ = lv_obj_create(bar);
    lv_obj_set_size(scan_track_, kScanTrackW, kScanTrackH);
    lv_obj_align(scan_track_, LV_ALIGN_LEFT_MID, 330, 0);
    lv_obj_set_style_bg_color(scan_track_, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_border_color(scan_track_, lv_color_hex(COL_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(scan_track_, 1, 0);
    lv_obj_set_style_radius(scan_track_, kScanTrackH / 2, 0);
    lv_obj_set_style_pad_all(scan_track_, 0, 0);
    lv_obj_clear_flag(scan_track_, LV_OBJ_FLAG_SCROLLABLE);

    scan_puck_ = lv_obj_create(scan_track_);
    lv_obj_set_size(scan_puck_, kScanPuckW, kScanTrackH - 4);
    lv_obj_set_pos(scan_puck_, 2, 1);
    lv_obj_set_style_bg_color(scan_puck_, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_border_width(scan_puck_, 0, 0);
    lv_obj_set_style_radius(scan_puck_, (kScanTrackH - 4) / 2, 0);
    lv_obj_clear_flag(scan_puck_, LV_OBJ_FLAG_SCROLLABLE);

    auto make_pod = [&bar](int offset, const char *text) {
        lv_obj_t *label = lv_label_create(bar);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(label, font_mono(), 0);
        lv_obj_align(label, LV_ALIGN_RIGHT_MID, offset, 0);
        return label;
    };
    kbd_chip_ = make_pod(-430, "KB --");
    wifi_chip_ = make_pod(-300, "WIFI --");
    gw_chip_ = make_pod(-205, "GW --");
    bat_chip_ = make_pod(-105, "BAT --");
    clock_chip_ = make_pod(0, "--:--:--");
}

// ---- hero ---------------------------------------------------------------

void HermesConsoleScreen::build_hero(lv_obj_t *parent)
{
    lv_obj_t *hero = make_panel(parent, "AGENT", kRailX, kBodyY, kRailW, 236);
    add_corner_brackets(hero);

    // The real GLB-derived guardian replaces the Command Centre's old
    // five-line ASCII ARCHIE banner. Its canvas is bounded to this rail.
    archie_ = archie_pointcloud_create(hero, 300, 164, COL_PANEL, true);
    if (archie_ != nullptr) {
        lv_obj_align(archie_->canvas, LV_ALIGN_TOP_MID, 0, -4);
        lv_obj_add_flag(archie_->canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(archie_->canvas, [](lv_event_t *e) {
            auto *self = static_cast<HermesConsoleScreen *>(lv_event_get_user_data(e));
            archie_pointcloud_pulse(self->archie());
        }, LV_EVENT_CLICKED, this);
    }

    persona_label_ = lv_label_create(hero);
    lv_label_set_text(persona_label_, "ARCHIE // NVS GUARDIAN");
    lv_obj_set_style_text_color(persona_label_, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_text_font(persona_label_, font_mono(), 0);
    lv_obj_align(persona_label_, LV_ALIGN_BOTTOM_MID, 0, -30);

    state_label_ = lv_label_create(hero);
    lv_label_set_text(state_label_, "state: linking // messages: 0");
    lv_obj_set_style_text_color(state_label_, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(state_label_, font_mono(), 0);
    lv_obj_align(state_label_, LV_ALIGN_BOTTOM_MID, 0, -4);
}

// ---- overview -------------------------------------------------------------

void HermesConsoleScreen::build_overview(lv_obj_t *parent)
{
    const int link_y = kBodyY + 250;
    const int link_h = kDeckY - link_y - kPad;
    lv_obj_t *link = make_panel(parent, "LINK", kRailX, link_y, kRailW, link_h);

    lv_obj_t *host = lv_label_create(link);
    lv_label_set_text(host, "> tab5:~ _");
    lv_obj_set_style_text_color(host, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(host, font_mono(), 0);
    lv_obj_set_pos(host, 0, 2);

    auto make_pill = [&link](const char *text, int x) {
        lv_obj_t *pill = lv_label_create(link);
        lv_label_set_text(pill, text);
        lv_obj_set_style_text_font(pill, font_mono(), 0);
        lv_obj_set_style_text_color(pill, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(COL_PANEL_EDGE), 0);
        lv_obj_set_style_border_width(pill, 1, 0);
        lv_obj_set_style_radius(pill, 4, 0);
        lv_obj_set_style_pad_hor(pill, 8, 0);
        lv_obj_set_style_pad_ver(pill, 3, 0);
        lv_obj_set_pos(pill, x, 38);
        return pill;
    };
    net_pill_ = make_pill("NET", 0);
    gw_pill_ = make_pill("GW", 108);
    ai_pill_ = make_pill("AI", 216);

    lv_obj_t *trace = lv_label_create(link);
    lv_label_set_text(trace, "--o----------o--");
    lv_obj_set_style_text_color(trace, lv_color_hex(COL_PANEL_EDGE), 0);
    lv_obj_set_style_text_font(trace, font_mono(), 0);
    lv_obj_align(trace, LV_ALIGN_TOP_MID, 0, 82);

    auto make_gauge = [&link](const char *key, int y, lv_obj_t **bar, lv_obj_t **value) {
        lv_obj_t *label = lv_label_create(link);
        lv_label_set_text(label, key);
        lv_obj_set_style_text_color(label, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(label, font_mono(), 0);
        lv_obj_set_pos(label, 0, y);

        *bar = lv_bar_create(link);
        lv_obj_set_size(*bar, 112, 10);
        lv_obj_set_pos(*bar, 56, y + 6);
        lv_bar_set_range(*bar, 0, 100);
        lv_obj_set_style_bg_color(*bar, lv_color_hex(COL_PANEL_EDGE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(*bar, lv_color_hex(COL_SUN), LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_color(*bar, lv_color_hex(COL_YELLOW), LV_PART_INDICATOR);
        lv_obj_set_style_bg_grad_dir(*bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);

        *value = lv_label_create(link);
        lv_label_set_text(*value, "--");
        lv_obj_set_style_text_color(*value, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_text_font(*value, font_mono(), 0);
        lv_obj_set_pos(*value, 182, y);
    };
    make_gauge("sig", 108, &sig_bar_, &ov_signal_);
    make_gauge("heap", 142, &heap_bar_, &ov_session_);
    make_gauge("tok", 176, &token_bar_, &ov_tokens_);
    make_gauge("bat", 210, &bat_bar_, &ov_battery_);

    lv_obj_t *up_key = lv_label_create(link);
    lv_label_set_text(up_key, "up");
    lv_obj_set_style_text_color(up_key, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(up_key, font_mono(), 0);
    lv_obj_set_pos(up_key, 0, 250);
    ov_uptime_ = lv_label_create(link);
    lv_label_set_text(ov_uptime_, "--:--:--");
    lv_obj_set_style_text_color(ov_uptime_, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(ov_uptime_, font_mono(), 0);
    lv_obj_set_pos(ov_uptime_, 56, 250);

    lv_obj_t *tasks_key = lv_label_create(link);
    lv_label_set_text(tasks_key, "tasks");
    lv_obj_set_style_text_color(tasks_key, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(tasks_key, font_mono(), 0);
    lv_obj_set_pos(tasks_key, 176, 250);
    tasks_value_ = lv_label_create(link);
    lv_label_set_text(tasks_value_, "0/0");
    lv_obj_set_style_text_color(tasks_value_, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(tasks_value_, font_mono(), 0);
    lv_obj_set_pos(tasks_value_, 242, 250);
}

// ---- chat -------------------------------------------------------------------

void HermesConsoleScreen::build_chat(lv_obj_t *parent)
{
    const int log_h = kDeckY - kBodyY - kInputH - 2 * kPad;
    chat_list_ = make_panel(parent, "CONVERSATION", kLogX, kBodyY, kLogW, log_h);
    lv_obj_set_flex_flow(chat_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(chat_list_, 4, 0);
    lv_obj_add_flag(chat_list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(chat_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(chat_list_, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(chat_list_, lv_color_hex(COL_SUN), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(chat_list_, LV_OPA_60, LV_PART_SCROLLBAR);

    // Clear tag sits on the terminal frame, not inside its scrolling content.
    lv_obj_t *clear = lv_label_create(parent);
    lv_label_set_text(clear, "X CLEAR");
    lv_obj_set_style_text_color(clear, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(clear, font_mono(), 0);
    lv_obj_set_style_bg_color(clear, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(clear, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(clear, 6, 0);
    lv_obj_set_pos(clear, kLogX + kLogW - 112, kBodyY - 9);
    lv_obj_add_flag(clear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(clear, 14);
    lv_obj_set_user_data(clear, reinterpret_cast<void *>(ACT_CLEAR));
    lv_obj_add_event_cb(clear, action_event_cb, LV_EVENT_CLICKED, this);

    input_row_ = lv_obj_create(parent);
    lv_obj_set_pos(input_row_, kLogX, kBodyY + log_h + kPad);
    lv_obj_set_size(input_row_, kLogW, kInputH);
    lv_obj_set_style_bg_color(input_row_, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(input_row_, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_border_width(input_row_, 1, 0);
    lv_obj_set_style_border_opa(input_row_, LV_OPA_40, 0);
    lv_obj_set_style_radius(input_row_, 4, 0);
    lv_obj_set_style_pad_hor(input_row_, 10, 0);
    lv_obj_set_style_pad_ver(input_row_, 2, 0);
    lv_obj_clear_flag(input_row_, LV_OBJ_FLAG_SCROLLABLE);

    input_label_ = lv_label_create(input_row_);
    lv_obj_set_width(input_label_, kLogW - 28);
    lv_label_set_long_mode(input_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(input_label_, font_mono(), 0);
    lv_obj_align(input_label_, LV_ALIGN_LEFT_MID, 0, 0);
    refresh_input_line();
}

// ---- tasks -----------------------------------------------------------------

void HermesConsoleScreen::build_tasks(lv_obj_t *parent)
{
    (void)parent;
}

void HermesConsoleScreen::set_tasks(const HermesTaskItem *items, int count)
{
    task_count_ = count < 0 ? 0 : count;
    task_done_ = 0;
    for (int i = 0; i < task_count_; ++i) {
        if (items != nullptr && items[i].done) {
            ++task_done_;
        }
    }
    if (tasks_value_ != nullptr) {
        char value[24];
        std::snprintf(value, sizeof(value), "%d/%d", task_done_, task_count_);
        lv_label_set_text(tasks_value_, value);
        lv_obj_set_style_text_color(tasks_value_,
                                    lv_color_hex(task_count_ > task_done_ ? COL_YELLOW : COL_TEXT), 0);
    }
}

// ---- actions ---------------------------------------------------------------

void HermesConsoleScreen::build_actions(lv_obj_t *parent)
{
    lv_obj_t *deck = lv_obj_create(parent);
    lv_obj_set_pos(deck, kPad, kDeckY);
    lv_obj_set_size(deck, LCD_W - 2 * kPad, kDeckH);
    lv_obj_set_style_bg_opa(deck, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(deck, 0, 0);
    lv_obj_set_style_pad_all(deck, 0, 0);
    lv_obj_set_flex_flow(deck, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(deck, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(deck, LV_OBJ_FLAG_SCROLLABLE);

    struct Spec {
        const char *text;
        ActionId id;
        uint32_t accent;
    };
    const Spec specs[] = {
        {"* BRIEF", ACT_BRIEF, COL_SUN},
        {"# TASKS", ACT_TASKS, COL_SUN},
        {"PERSONA: HERMES", ACT_PERSONA, COL_AMBER},
        {"X CLEAR", ACT_CLEAR, COL_DIM},
        {"> SET", ACT_SETTINGS, COL_DIM},
        {"[] STOP", ACT_STOP, COL_ALERT},
    };
    for (const Spec &spec : specs) {
        lv_obj_t *button = make_deck_button(deck, spec.text, spec.accent);
        lv_obj_set_size(button, 196, kDeckH - 8);
        lv_obj_set_user_data(button, reinterpret_cast<void *>(spec.id));
        lv_obj_add_event_cb(button, action_event_cb, LV_EVENT_CLICKED, this);
        if (spec.id == ACT_PERSONA) {
            btn_persona_ = button;
            btn_persona_label_ = lv_obj_get_child(button, 0);
        }
    }
}

void HermesConsoleScreen::action_event_cb(lv_event_t *e)
{
    auto *self = static_cast<HermesConsoleScreen *>(lv_event_get_user_data(e));
    auto *target = static_cast<lv_obj_t *>(lv_event_get_target(e));
    auto id = static_cast<ActionId>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    switch (id) {
    case ACT_BRIEF: {
        constexpr const char *prompt = "Give me a brief status update in 2-3 sentences.";
        self->append_user_message(prompt);
        if (self->actions_.send_chat != nullptr) {
            self->actions_.send_chat(self->active_persona(), prompt, self->actions_.ctx);
        }
        break;
    }
    case ACT_TASKS: {
        constexpr const char *prompt = "What are my current priorities and open tasks?";
        self->append_user_message(prompt);
        if (self->actions_.send_chat != nullptr) {
            self->actions_.send_chat(self->active_persona(), prompt, self->actions_.ctx);
        }
        break;
    }
    case ACT_PERSONA:
        self->cycle_persona();
        break;
    case ACT_SETTINGS:
        if (self->actions_.open_settings != nullptr) {
            self->actions_.open_settings(self->actions_.ctx);
        }
        break;
    case ACT_STOP:
        if (self->actions_.stop_generation != nullptr) {
            self->actions_.stop_generation(self->actions_.ctx);
        }
        break;
    case ACT_CLEAR:
        self->clear_chat();
        break;
    }
}

void HermesConsoleScreen::clear_chat()
{
    lv_obj_clean(chat_list_);
    log_entries_ = 0;
    reply_label_ = nullptr;
    reply_text_.clear();
    session_msgs_ = 0;
    append_log("chat cleared", COL_DIM);
    if (actions_.clear_session != nullptr) {
        actions_.clear_session(active_persona(), actions_.ctx);
    }
}

// ---- persona ---------------------------------------------------------------

void HermesConsoleScreen::set_persona(const char *name)
{
    if (name == nullptr || name[0] == '\0') {
        return;
    }
    for (size_t i = 0; i < personas_.size(); ++i) {
        if (personas_[i] == name) {
            persona_index_ = i;
            apply_persona_visuals();
            return;
        }
    }
    personas_.push_back(name);
    persona_index_ = personas_.size() - 1;
    apply_persona_visuals();
}

void HermesConsoleScreen::cycle_persona()
{
    persona_index_ = (persona_index_ + 1) % personas_.size();
    apply_persona_visuals();
    char line[64];
    std::snprintf(line, sizeof(line), "persona -> %s", active_persona());
    append_log(line, COL_DIM);
}

void HermesConsoleScreen::apply_persona_visuals()
{
    const std::string &active = personas_[persona_index_];
    uint32_t accent = persona_accent(active.c_str());
    if (persona_label_ != nullptr) {
        std::string pretty = active;
        for (char &c : pretty) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        pretty += " // NVS GUARDIAN";
        lv_label_set_text(persona_label_, pretty.c_str());
        lv_obj_set_style_text_color(persona_label_, lv_color_hex(accent), 0);
    }
    if (btn_persona_label_ != nullptr) {
        const std::string &next = personas_[(persona_index_ + 1) % personas_.size()];
        std::string next_text = "PERSONA: ";
        for (char c : next) {
            next_text += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
        }
        lv_label_set_text(btn_persona_label_, next_text.c_str());
        lv_obj_set_style_text_color(btn_persona_label_, lv_color_hex(persona_accent(next.c_str())), 0);
    }
}

// ---- chat content ------------------------------------------------------------

lv_obj_t *HermesConsoleScreen::new_chat_label(uint32_t color)
{
    lv_obj_t *label = lv_label_create(chat_list_);
    lv_obj_set_width(label, kLogW - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font_mono(), 0);
    ++log_entries_;
    trim_log();
    return label;
}

void HermesConsoleScreen::trim_log()
{
    while (log_entries_ > kMaxLogEntries) {
        lv_obj_t *oldest = lv_obj_get_child(chat_list_, 0);
        if (oldest == nullptr) {
            log_entries_ = 0;
            return;
        }
        if (oldest == reply_label_) {
            reply_label_ = nullptr;
        }
        lv_obj_delete(oldest);
        --log_entries_;
    }
}

void HermesConsoleScreen::append_log(const char *text, uint32_t color)
{
    lv_obj_t *label = new_chat_label(color);
    lv_label_set_text(label, ascii_sanitize(text).c_str());
    lv_obj_scroll_to_view(label, LV_ANIM_OFF);
}

void HermesConsoleScreen::append_user_message(const char *text)
{
    std::string line = "YOU > ";
    line += ascii_sanitize(text);
    lv_obj_t *label = new_chat_label(COL_YELLOW);
    lv_label_set_text(label, line.c_str());
    lv_obj_scroll_to_view(label, LV_ANIM_OFF);
    ++session_msgs_;
}

void HermesConsoleScreen::on_chat_text(const char *persona, const char *text, bool done)
{
    if (reply_label_ == nullptr) {
        const char *name = (persona != nullptr && persona[0] != '\0') ? persona : active_persona();
        uint32_t accent = persona_accent(name);
        reply_text_ = name;
        for (char &c : reply_text_) {
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
        }
        reply_text_ += " > ";
        reply_label_ = new_chat_label(COL_TEXT);
        lv_obj_set_style_border_side(reply_label_, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(reply_label_, lv_color_hex(accent), 0);
        lv_obj_set_style_border_width(reply_label_, 2, 0);
        lv_obj_set_style_pad_left(reply_label_, 8, 0);
        lv_obj_set_style_text_color(reply_label_, lv_color_hex(accent), 0);
        lv_label_set_text(reply_label_, reply_text_.c_str());
    }
    if (text != nullptr && text[0] != '\0') {
        reply_text_ += ascii_sanitize(text);
        lv_label_set_text(reply_label_, reply_text_.c_str());
        lv_obj_scroll_to_view(reply_label_, LV_ANIM_OFF);
        archie_pointcloud_pulse(archie_);
    }
    if (done) {
        reply_label_ = nullptr;
        reply_text_.clear();
        ++session_msgs_;
    }
}

// ---- telemetry -----------------------------------------------------------------

void HermesConsoleScreen::update_telemetry(const TelemetrySnapshot &snap)
{
    char buf[96];

    // Clock pod. Before SNTP is ready, uptime is still useful and avoids
    // displaying a plausible-but-wrong wall clock.
    std::time_t now = std::time(nullptr);
    std::tm tm_now = {};
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 >= 2025) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                      tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    } else {
        std::snprintf(buf, sizeof(buf), "UP %02u:%02u",
                      static_cast<unsigned>(snap.uptime_sec / 3600),
                      static_cast<unsigned>((snap.uptime_sec / 60) % 60));
    }
    if (last_clock_ != buf) {
        last_clock_ = buf;
        lv_label_set_text(clock_chip_, buf);
    }

    auto style_pill = [](lv_obj_t *pill, bool live, uint32_t live_color) {
        uint32_t color = live ? live_color : COL_DIM;
        lv_obj_set_style_text_color(pill, lv_color_hex(color), 0);
        lv_obj_set_style_border_color(pill,
                                      lv_color_hex(live ? live_color : COL_PANEL_EDGE), 0);
    };

    // Network pod, first link pill and signal gauge.
    if (!net_drawn_once_ || last_net_up_ != snap.wifi_connected) {
        net_drawn_once_ = true;
        last_net_up_ = snap.wifi_connected;
        style_pill(net_pill_, snap.wifi_connected, COL_YELLOW);
    }
    const bool valid_rssi = snap.wifi_connected &&
                            snap.wifi_rssi_dbm < 0 && snap.wifi_rssi_dbm >= -90;
    if (valid_rssi) {
        std::snprintf(buf, sizeof(buf), "WIFI %d", snap.wifi_rssi_dbm);
    } else {
        std::snprintf(buf, sizeof(buf), "WIFI --");
    }
    if (last_wifi_ != buf) {
        last_wifi_ = buf;
        lv_label_set_text(wifi_chip_, buf);
        lv_obj_set_style_text_color(wifi_chip_,
                                    lv_color_hex(valid_rssi ? COL_YELLOW : COL_DIM), 0);
    }
    int signal_percent = valid_rssi
                             ? std::clamp((snap.wifi_rssi_dbm + 90) * 2, 0, 100)
                             : 0;
    lv_bar_set_value(sig_bar_, signal_percent, LV_ANIM_OFF);
    if (valid_rssi) {
        std::snprintf(buf, sizeof(buf), "%d dBm", snap.wifi_rssi_dbm);
    } else {
        std::snprintf(buf, sizeof(buf), "--");
    }
    if (last_ov_signal_ != buf) {
        last_ov_signal_ = buf;
        lv_label_set_text(ov_signal_, buf);
    }

    // Gateway pod and second link pill. Endpoint details never appear here,
    // which keeps screenshots of the open-source UI safe to share.
    if (!gw_drawn_once_ || last_gw_connected_ != snap.gateway_connected) {
        gw_drawn_once_ = true;
        last_gw_connected_ = snap.gateway_connected;
        lv_label_set_text(gw_chip_, snap.gateway_connected ? "GW UP" : "GW --");
        lv_obj_set_style_text_color(gw_chip_,
                                    lv_color_hex(snap.gateway_connected ? COL_YELLOW : COL_DIM), 0);
        style_pill(gw_pill_, snap.gateway_connected, COL_YELLOW);
    }

    // Physical-keyboard pod.
    bool kbd = keyboard_status_get();
    if (!kbd_drawn_once_ || kbd != last_kbd_) {
        kbd_drawn_once_ = true;
        last_kbd_ = kbd;
        lv_label_set_text(kbd_chip_, kbd ? "KB ON" : "KB --");
        lv_obj_set_style_text_color(kbd_chip_, lv_color_hex(kbd ? COL_YELLOW : COL_DIM), 0);
    }

    // Battery pod and gauge. Unknown battery means external/USB power, not a
    // fabricated percentage.
    if (snap.battery_percent < 0) {
        std::snprintf(buf, sizeof(buf), "BAT USB");
    } else {
        std::snprintf(buf, sizeof(buf), "BAT %d%%", snap.battery_percent);
    }
    if (last_bat_ != buf) {
        last_bat_ = buf;
        lv_label_set_text(bat_chip_, buf);
        bool low = snap.battery_percent >= 0 && snap.battery_percent <= 20;
        lv_obj_set_style_text_color(bat_chip_,
                                    lv_color_hex(low ? COL_ALERT : COL_YELLOW), 0);
    }
    if (snap.battery_percent < 0) {
        std::snprintf(buf, sizeof(buf), "USB");
    } else {
        std::snprintf(buf, sizeof(buf), "%.1fV %d%%", static_cast<double>(snap.battery_volts),
                      snap.battery_percent);
    }
    lv_bar_set_value(bat_bar_, snap.battery_percent < 0 ? 0 : snap.battery_percent, LV_ANIM_OFF);
    if (last_ov_battery_ != buf) {
        last_ov_battery_ = buf;
        lv_label_set_text(ov_battery_, buf);
    }

    // Internal heap is the constrained pool; the value also exposes PSRAM,
    // where the display and character buffers live.
    unsigned long heap_kb = static_cast<unsigned long>(snap.free_heap_kb);
    unsigned long psram_mb = static_cast<unsigned long>(snap.free_psram_kb / 1024);
    int heap_percent = static_cast<int>(std::min<unsigned long>(heap_kb, 512) * 100 / 512);
    lv_bar_set_value(heap_bar_, heap_percent, LV_ANIM_OFF);
    std::snprintf(buf, sizeof(buf), "%luK/%luM", heap_kb, psram_mb);
    if (last_ov_session_ != buf) {
        last_ov_session_ = buf;
        lv_label_set_text(ov_session_, buf);
        lv_obj_set_style_text_color(ov_session_,
                                    lv_color_hex(heap_kb < 60 ? COL_ALERT : COL_TEXT), 0);
    }

    // Session tokens use a bounded load bar and compact in/out readout.
    auto fmt_k = [](unsigned long v, char *out, size_t n) {
        if (v >= 10000) {
            std::snprintf(out, n, "%luk", v / 1000);
        } else {
            std::snprintf(out, n, "%lu", v);
        }
    };
    char tin[12], tout[12];
    fmt_k(static_cast<unsigned long>(snap.tokens_in), tin, sizeof(tin));
    fmt_k(static_cast<unsigned long>(snap.tokens_out), tout, sizeof(tout));
    unsigned long token_total = static_cast<unsigned long>(snap.tokens_in) +
                                static_cast<unsigned long>(snap.tokens_out);
    lv_bar_set_value(token_bar_,
                     static_cast<int>(std::min<unsigned long>(token_total, 10000) / 100),
                     LV_ANIM_OFF);
    std::snprintf(buf, sizeof(buf), "%s/%s", tin, tout);
    if (last_ov_tokens_ != buf) {
        last_ov_tokens_ = buf;
        lv_label_set_text(ov_tokens_, buf);
    }
    std::snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
                  static_cast<unsigned>(snap.uptime_sec / 3600),
                  static_cast<unsigned>((snap.uptime_sec / 60) % 60),
                  static_cast<unsigned>(snap.uptime_sec % 60));
    if (last_ov_uptime_ != buf) {
        last_ov_uptime_ = buf;
        lv_label_set_text(ov_uptime_, buf);
    }

    // AI pill, scanner and particle guardian share one state truth. The scan
    // animation exists only while work is active, keeping idle redraws cheap.
    bool thinking = snap.agent_state == "thinking" || snap.agent_state == "speaking";
    const char *state_name;
    uint32_t state_color;
    if (!snap.gateway_connected) {
        state_name = "linking";
        state_color = COL_DIM;
    } else if (snap.agent_state == "thinking") {
        state_name = "thinking";
        state_color = COL_AMBER;
    } else if (snap.agent_state == "speaking") {
        state_name = "speaking";
        state_color = COL_YELLOW;
    } else if (snap.agent_state == "error") {
        state_name = "error";
        state_color = COL_ALERT;
    } else {
        state_name = "online";
        state_color = COL_OK;
    }
    std::snprintf(buf, sizeof(buf), "state: %s // msg: %d", state_name, session_msgs_);
    if (last_state_ != buf) {
        last_state_ = buf;
        lv_label_set_text(state_label_, buf);
        lv_obj_set_style_text_color(state_label_, lv_color_hex(state_color), 0);
    }
    style_pill(ai_pill_, snap.gateway_connected,
               snap.agent_state == "error" ? COL_ALERT
               : snap.agent_state == "speaking" ? COL_YELLOW
               : snap.agent_state == "thinking" ? COL_AMBER
                                                   : COL_OK);
    if (thinking != last_thinking_) {
        last_thinking_ = thinking;
        lv_anim_delete(scan_puck_, scan_anim_cb);
        if (thinking) {
            lv_obj_set_style_bg_color(scan_puck_, lv_color_hex(state_color), 0);
            lv_anim_t sweep;
            lv_anim_init(&sweep);
            lv_anim_set_var(&sweep, scan_puck_);
            lv_anim_set_exec_cb(&sweep, scan_anim_cb);
            lv_anim_set_values(&sweep, 2, kScanTrackW - kScanPuckW - 2);
            lv_anim_set_duration(&sweep, 720);
            lv_anim_set_playback_duration(&sweep, 720);
            lv_anim_set_repeat_count(&sweep, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&sweep, lv_anim_path_ease_in_out);
            lv_anim_start(&sweep);
        } else {
            lv_obj_set_x(scan_puck_, 2);
            lv_obj_set_style_bg_color(scan_puck_, lv_color_hex(COL_DIM), 0);
        }
    }
    ArchieVisualState visual = ArchieVisualState::Idle;
    if (!snap.gateway_connected) {
        visual = ArchieVisualState::Linking;
    } else if (snap.agent_state == "thinking" || snap.agent_state == "speaking") {
        visual = ArchieVisualState::Thinking;
    } else if (snap.agent_state == "error") {
        visual = ArchieVisualState::Error;
    } else {
        visual = ArchieVisualState::Online;
    }
    archie_pointcloud_set_state(archie_, visual);
}

// ---- input -----------------------------------------------------------------

void HermesConsoleScreen::refresh_input_line()
{
    lv_obj_set_style_border_color(input_row_,
                                  lv_color_hex(input_buffer_.empty() ? COL_SUN : COL_YELLOW), 0);
    lv_obj_set_style_border_opa(input_row_, input_buffer_.empty() ? LV_OPA_40 : LV_OPA_COVER, 0);
    if (input_buffer_.empty()) {
        std::string prompt = "tab5:~ > message ";
        prompt += active_persona();
        prompt += " + Enter_";
        lv_label_set_text(input_label_, prompt.c_str());
        lv_obj_set_style_text_color(input_label_, lv_color_hex(COL_DIM), 0);
        return;
    }
    std::string shown = "tab5:~ > ";
    shown += input_buffer_;
    shown += "_";
    lv_obj_set_style_text_color(input_label_, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(input_label_, shown.c_str());
}

void HermesConsoleScreen::submit_input()
{
    if (input_buffer_.empty()) {
        return;
    }
    if (input_buffer_ == "/test") {
        input_buffer_.clear();
        refresh_input_line();
        if (actions_.run_self_test != nullptr) {
            actions_.run_self_test(actions_.ctx);
        }
        return;
    }
    append_user_message(input_buffer_.c_str());
    if (actions_.send_chat != nullptr) {
        actions_.send_chat(active_persona(), input_buffer_.c_str(), actions_.ctx);
    }
    input_buffer_.clear();
    refresh_input_line();
}

void HermesConsoleScreen::key_input(const uint8_t *data, size_t len)
{
    bool dirty = false;
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        if (byte == '\r' || byte == '\n') {
            submit_input();
            continue;
        }
        if (byte == 0x08 || byte == 0x7f) {
            if (!input_buffer_.empty()) {
                input_buffer_.pop_back();
                dirty = true;
            }
            continue;
        }
        if (byte == 0x1b) {
            input_buffer_.clear();
            dirty = true;
            continue;
        }
        if (byte >= 0x20 && byte < 0x7f && input_buffer_.size() < 512) {
            input_buffer_.push_back(static_cast<char>(byte));
            dirty = true;
        }
    }
    if (dirty) {
        refresh_input_line();
    }
}
