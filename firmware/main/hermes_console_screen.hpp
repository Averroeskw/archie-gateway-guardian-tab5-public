#pragma once

#include "archie_pointcloud.hpp"
#include "telemetry_model.hpp"
#include "ws_hermes_client.hpp"

#include "lvgl.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Archie Command Centre: the proven two-column terminal UI from the private
// Tab5 Agent OS, with its ASCII banner replaced by the GLB-derived particle
// guardian. Fixed 1280x720 landscape:
//
//   status bar    COMMAND CENTRE + activity scanner + device pods
//   left rail     animated guardian + live link/resource gauges
//   terminal      streaming conversation + physical-keyboard input
//   command deck  brief · tasks · persona · clear · settings · stop
//
// Threading contract unchanged: every method runs while holding
// bsp_display_lock(); producers marshal through the lock; periodic refresh
// runs on an LVGL timer owned by the mode glue.
struct ConsoleActions {
    void (*send_chat)(const char *persona, const char *text, void *ctx);
    void (*stop_generation)(void *ctx);
    void (*open_settings)(void *ctx);
    // Typed as /test: link + device-health self-test.
    void (*run_self_test)(void *ctx);
    // Chat cleared: the owner tells the gateway to drop this persona's
    // conversation memory.
    void (*clear_session)(const char *persona, void *ctx);
    void *ctx;
};

class HermesConsoleScreen {
public:
    void create(lv_indev_t *touch_indev, const ConsoleActions &actions);
    lv_obj_t *screen() { return screen_; }

    // Session log line (system/gateway events) in the terminal.
    void append_log(const char *text, uint32_t color);
    // User-authored message echoed into the chat when sent.
    void append_user_message(const char *text);
    // Streaming assistant text; done closes the current terminal turn.
    void on_chat_text(const char *persona, const char *text, bool done);
    // Snapshot for the LINK rail task counter (from a `tasks` envelope).
    void set_tasks(const HermesTaskItem *items, int count);

    // Pull-based refresh from TelemetryModel (called by an LVGL timer).
    void update_telemetry(const TelemetrySnapshot &snap);

    // Bytes from the physical keyboard (printables, \r, \b, ESC).
    void key_input(const uint8_t *data, size_t len);

    // Select the active persona by name. Unknown names join the cycle as a
    // custom persona (white accent).
    void set_persona(const char *name);

    const char *active_persona() const { return personas_[persona_index_].c_str(); }
    ArchiePointCloud *archie() { return archie_; }

private:
    void build_header(lv_obj_t *parent);    // status bar
    void build_hero(lv_obj_t *parent);      // AGENT rail
    void build_overview(lv_obj_t *parent);  // LINK rail
    void build_chat(lv_obj_t *parent);      // conversation terminal
    void build_tasks(lv_obj_t *parent);     // retained API; task summary lives in LINK
    void build_actions(lv_obj_t *parent);   // command deck
    void refresh_input_line();
    void submit_input();
    void cycle_persona();
    void apply_persona_visuals();
    void clear_chat();
    void trim_log();
    lv_obj_t *new_chat_label(uint32_t color);

    static void action_event_cb(lv_event_t *e);

    ConsoleActions actions_ = {};
    lv_obj_t *screen_ = nullptr;

    // Status bar
    lv_obj_t *scan_track_ = nullptr;
    lv_obj_t *scan_puck_ = nullptr;
    lv_obj_t *wifi_chip_ = nullptr;
    lv_obj_t *gw_chip_ = nullptr;
    lv_obj_t *kbd_chip_ = nullptr;
    lv_obj_t *bat_chip_ = nullptr;
    lv_obj_t *clock_chip_ = nullptr;

    // AGENT panel
    ArchiePointCloud *archie_ = nullptr;
    lv_obj_t *persona_label_ = nullptr;
    lv_obj_t *state_label_ = nullptr;

    // LINK panel
    lv_obj_t *net_pill_ = nullptr;
    lv_obj_t *gw_pill_ = nullptr;
    lv_obj_t *ai_pill_ = nullptr;
    lv_obj_t *sig_bar_ = nullptr;
    lv_obj_t *heap_bar_ = nullptr;
    lv_obj_t *token_bar_ = nullptr;
    lv_obj_t *bat_bar_ = nullptr;
    lv_obj_t *ov_battery_ = nullptr;
    lv_obj_t *ov_signal_ = nullptr;
    lv_obj_t *ov_tokens_ = nullptr;
    lv_obj_t *ov_session_ = nullptr;
    lv_obj_t *ov_uptime_ = nullptr;
    lv_obj_t *tasks_value_ = nullptr;

    // Conversation terminal
    lv_obj_t *chat_list_ = nullptr;
    lv_obj_t *input_row_ = nullptr;
    lv_obj_t *input_label_ = nullptr;
    lv_obj_t *reply_label_ = nullptr;  // open streaming turn, else nullptr
    std::string reply_text_;
    std::string input_buffer_;

    // Command deck
    lv_obj_t *btn_persona_ = nullptr;
    lv_obj_t *btn_persona_label_ = nullptr;

    // Cached last-rendered values (repaint only what changed).
    std::string last_wifi_;
    std::string last_bat_;
    std::string last_clock_;
    std::string last_state_;
    std::string last_ov_battery_;
    std::string last_ov_signal_;
    std::string last_ov_tokens_;
    std::string last_ov_session_;
    std::string last_ov_uptime_;
    bool last_gw_connected_ = false;
    bool gw_drawn_once_ = false;
    bool last_net_up_ = false;
    bool net_drawn_once_ = false;
    bool last_kbd_ = false;
    bool kbd_drawn_once_ = false;
    bool last_thinking_ = false;
    int session_msgs_ = 0;
    int task_count_ = 0;
    int task_done_ = 0;

    std::vector<std::string> personas_{"archie", "hermes", "openclaw"};
    size_t persona_index_ = 0;
    int log_entries_ = 0;
};
