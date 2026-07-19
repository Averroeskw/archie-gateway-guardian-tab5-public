#include "tab5_keyboard.hpp"
#include "app_config.hpp"

#include <cstring>

#ifndef TAB5_KEYBOARD_HOST_TEST
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

namespace {

#ifndef TAB5_KEYBOARD_HOST_TEST
constexpr const char *TAG = "tab5kbd";
constexpr uint8_t TAB5_KBD_ADDR = 0x6D;
constexpr i2c_port_num_t TAB5_KBD_I2C_PORT = I2C_NUM_0;
constexpr gpio_num_t TAB5_KBD_SDA = GPIO_NUM_0;
constexpr gpio_num_t TAB5_KBD_SCL = GPIO_NUM_1;
constexpr gpio_num_t TAB5_KBD_INT = GPIO_NUM_50;
constexpr uint8_t REG_EVENT_NUM = 0x02;
constexpr uint8_t REG_KEYBOARD_MODE = 0x10;
constexpr uint8_t REG_HID_EVENT = 0x30;
constexpr uint8_t REG_RGB_COLOR = 0x60;
constexpr uint8_t REG_VERSION = 0xFE;
constexpr uint8_t MODE_HID = 1;

i2c_master_bus_handle_t s_bus = nullptr;
i2c_master_dev_handle_t s_dev = nullptr;
#endif

constexpr uint8_t HID_A = 0x04;
constexpr uint8_t HID_Z = 0x1D;
constexpr uint8_t HID_1 = 0x1E;
constexpr uint8_t HID_9 = 0x26;
constexpr uint8_t HID_0 = 0x27;
constexpr uint8_t HID_ENTER = 0x28;
constexpr uint8_t HID_ESC = 0x29;
constexpr uint8_t HID_BACKSPACE = 0x2A;
constexpr uint8_t HID_TAB = 0x2B;
constexpr uint8_t HID_SPACE = 0x2C;
constexpr uint8_t HID_MINUS = 0x2D;
constexpr uint8_t HID_EQUAL = 0x2E;
constexpr uint8_t HID_LBRACKET = 0x2F;
constexpr uint8_t HID_RBRACKET = 0x30;
constexpr uint8_t HID_BACKSLASH = 0x31;
constexpr uint8_t HID_SEMICOLON = 0x33;
constexpr uint8_t HID_APOSTROPHE = 0x34;
constexpr uint8_t HID_GRAVE = 0x35;
constexpr uint8_t HID_COMMA = 0x36;
constexpr uint8_t HID_DOT = 0x37;
constexpr uint8_t HID_SLASH = 0x38;
constexpr uint8_t HID_HOME = 0x4A;
constexpr uint8_t HID_PAGEUP = 0x4B;
constexpr uint8_t HID_DELETE = 0x4C;
constexpr uint8_t HID_END = 0x4D;
constexpr uint8_t HID_PAGEDOWN = 0x4E;
constexpr uint8_t HID_RIGHT = 0x4F;
constexpr uint8_t HID_LEFT = 0x50;
constexpr uint8_t HID_DOWN = 0x51;
constexpr uint8_t HID_UP = 0x52;
constexpr uint8_t MOD_CTRL = 0x11;
constexpr uint8_t MOD_SHIFT = 0x22;
constexpr uint8_t MOD_ALT = 0x44;

static bool append_byte(uint8_t *out, size_t out_cap, size_t &len, uint8_t byte)
{
    if (len >= out_cap) {
        return false;
    }
    out[len++] = byte;
    return true;
}

static bool append_cstr(uint8_t *out, size_t out_cap, size_t &len, const char *text)
{
    while (*text) {
        if (!append_byte(out, out_cap, len, static_cast<uint8_t>(*text++))) {
            return false;
        }
    }
    return true;
}

static char map_digit(uint8_t keycode, bool shift)
{
    static constexpr char normal[] = "1234567890";
    static constexpr char shifted[] = "!@#$%^&*()";
    if (keycode >= HID_1 && keycode <= HID_9) {
        const int idx = keycode - HID_1;
        return shift ? shifted[idx] : normal[idx];
    }
    if (keycode == HID_0) {
        return shift ? ')' : '0';
    }
    return 0;
}

static char map_symbol(uint8_t keycode, bool shift)
{
    switch (keycode) {
    case HID_MINUS:
        return shift ? '_' : '-';
    case HID_EQUAL:
        return shift ? '+' : '=';
    case HID_LBRACKET:
        return shift ? '{' : '[';
    case HID_RBRACKET:
        return shift ? '}' : ']';
    case HID_BACKSLASH:
        return shift ? '|' : '\\';
    case HID_SEMICOLON:
        return shift ? ':' : ';';
    case HID_APOSTROPHE:
        return shift ? '"' : '\'';
    case HID_GRAVE:
        return shift ? '~' : '`';
    case HID_COMMA:
        return shift ? '<' : ',';
    case HID_DOT:
        return shift ? '>' : '.';
    case HID_SLASH:
        return shift ? '?' : '/';
    default:
        return 0;
    }
}

} // namespace

size_t Tab5Keyboard::translate_hid(uint8_t modifier, uint8_t keycode, uint8_t *out, size_t out_cap)
{
    if (!out || out_cap == 0 || (modifier == 0 && keycode == 0)) {
        return 0;
    }

    size_t len = 0;
    const bool ctrl = (modifier & MOD_CTRL) != 0;
    const bool shift = (modifier & MOD_SHIFT) != 0;
    const bool alt = (modifier & MOD_ALT) != 0;

    auto emit_printable = [&](char c) {
        if (alt) {
            append_byte(out, out_cap, len, 0x1B);
        }
        append_byte(out, out_cap, len, static_cast<uint8_t>(c));
    };

    if (keycode >= HID_A && keycode <= HID_Z) {
        char c = static_cast<char>('a' + (keycode - HID_A));
        if (ctrl) {
            append_byte(out, out_cap, len, static_cast<uint8_t>((c - 'a') + 1));
            return len;
        }
        if (shift) {
            c = static_cast<char>('A' + (keycode - HID_A));
        }
        emit_printable(c);
        return len;
    }

    char digit = map_digit(keycode, shift);
    if (digit) {
        emit_printable(digit);
        return len;
    }

    char sym = map_symbol(keycode, shift);
    if (sym) {
        emit_printable(sym);
        return len;
    }

    switch (keycode) {
    case HID_ENTER:
        append_byte(out, out_cap, len, '\r');
        break;
    case HID_ESC:
        append_byte(out, out_cap, len, 0x1B);
        break;
    case HID_BACKSPACE:
        append_byte(out, out_cap, len, 0x7F);
        break;
    case HID_TAB:
        append_byte(out, out_cap, len, '\t');
        break;
    case HID_SPACE:
        emit_printable(' ');
        break;
    case HID_UP:
        append_cstr(out, out_cap, len, "\x1b[A");
        break;
    case HID_DOWN:
        append_cstr(out, out_cap, len, "\x1b[B");
        break;
    case HID_RIGHT:
        append_cstr(out, out_cap, len, "\x1b[C");
        break;
    case HID_LEFT:
        append_cstr(out, out_cap, len, "\x1b[D");
        break;
    case HID_DELETE:
        append_cstr(out, out_cap, len, "\x1b[3~");
        break;
    case HID_HOME:
        append_cstr(out, out_cap, len, "\x1b[H");
        break;
    case HID_END:
        append_cstr(out, out_cap, len, "\x1b[F");
        break;
    case HID_PAGEUP:
        append_cstr(out, out_cap, len, "\x1b[5~");
        break;
    case HID_PAGEDOWN:
        append_cstr(out, out_cap, len, "\x1b[6~");
        break;
    default:
        break;
    }
    return len;
}

#ifdef TAB5_KEYBOARD_HOST_TEST
bool Tab5Keyboard::begin() { return false; }
bool Tab5Keyboard::poll() { return false; }
bool Tab5Keyboard::set_rgb(uint8_t, uint8_t, uint8_t) { return false; }
const char *Tab5Keyboard::state_label() const { return "KBD OFFLINE"; }
bool Tab5Keyboard::write_reg(uint8_t, uint8_t) { return false; }
bool Tab5Keyboard::write_bytes(uint8_t, const uint8_t *, size_t) { return false; }
bool Tab5Keyboard::read_reg(uint8_t, uint8_t *, size_t) { return false; }
#else
bool Tab5Keyboard::begin()
{
    if (!s_bus) {
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = TAB5_KBD_I2C_PORT;
        bus_cfg.sda_io_num = TAB5_KBD_SDA;
        bus_cfg.scl_io_num = TAB5_KBD_SCL;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            if (!warned_offline_) {
                ESP_LOGW(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
                warned_offline_ = true;
            }
            ready_ = false;
            return false;
        }
    }
    if (!s_bus) {
        ready_ = false;
        return false;
    }

    if (!s_dev) {
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = TAB5_KBD_ADDR;
        dev_cfg.scl_speed_hz = 100000;
        esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
        if (err != ESP_OK) {
            if (!warned_offline_) {
                ESP_LOGW(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
                warned_offline_ = true;
            }
            ready_ = false;
            return false;
        }
    }

    gpio_config_t int_cfg = {};
    int_cfg.pin_bit_mask = 1ULL << TAB5_KBD_INT;
    int_cfg.mode = GPIO_MODE_INPUT;
    int_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    int_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    int_cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&int_cfg));

    uint8_t fw = 0;
    if (!read_reg(REG_VERSION, &fw, 1)) {
        if (!warned_offline_) {
            ESP_LOGW(TAG, "keyboard not ready at 0x%02X; retrying", TAB5_KBD_ADDR);
            warned_offline_ = true;
        }
        ready_ = false;
        return false;
    }
    ESP_LOGI(TAG, "found Tab5 keyboard at 0x%02X, fw=0x%02X", TAB5_KBD_ADDR, fw);

    if (!write_reg(REG_KEYBOARD_MODE, MODE_HID)) {
        ESP_LOGW(TAG, "failed to set HID mode; retrying");
        ready_ = false;
        return false;
    }
    ESP_LOGI(TAG, "keyboard mode=HID");
    ready_ = true;
    warned_offline_ = false;
    return true;
}

bool Tab5Keyboard::write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[] = {reg, value};
    return s_dev && i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100)) == ESP_OK;
}

bool Tab5Keyboard::write_bytes(uint8_t reg, const uint8_t *data, size_t len)
{
    if (!s_dev || !data || len == 0 || len > 8) {
        return false;
    }
    uint8_t buf[9] = {reg};
    std::memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, pdMS_TO_TICKS(100)) == ESP_OK;
}

bool Tab5Keyboard::read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return s_dev && data &&
           i2c_master_transmit_receive(s_dev, &reg, 1, data, len, pdMS_TO_TICKS(100)) == ESP_OK;
}

bool Tab5Keyboard::set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!ready_) {
        return false;
    }
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - last_rgb_ms_ < 750) {
        return true;
    }
    last_rgb_ms_ = now;
    uint8_t color[] = {red, green, blue};
    bool ok = write_bytes(REG_RGB_COLOR, color, sizeof(color));
    if (!ok) {
        ready_ = false;
        if (!warned_offline_) {
            ESP_LOGW(TAG, "failed to set keyboard RGB; disabling RGB until retry succeeds");
            warned_offline_ = true;
        }
    }
    return ok;
}

bool Tab5Keyboard::poll()
{
    if (!ready_) {
        return false;
    }

    // Self-heal: the A164 MCU can silently fall out of HID mode (green READY
    // only proves the I2C device ACKs, not that it still reports keys — seen
    // live as zero events with a healthy link). Periodically re-assert HID
    // mode and re-read the version; on failure drop ready_ so the task loop
    // runs a full begin(). A physical replug is only needed if even that fails.
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now_ms - last_maintain_ms_ >= kMaintainIntervalMs) {
        last_maintain_ms_ = now_ms;
        uint8_t fw = 0;
        if (!read_reg(REG_VERSION, &fw, 1) || !write_reg(REG_KEYBOARD_MODE, MODE_HID)) {
            ESP_LOGW(TAG, "keyboard maintenance failed; forcing re-init");
            ready_ = false;
            return false;
        }
    }

    uint8_t count = 0;
    if (!read_reg(REG_EVENT_NUM, &count, 1)) {
        ready_ = false;
        return false;
    }
    while (count-- > 0) {
        uint8_t hid[2] = {0, 0};
        if (!read_reg(REG_HID_EVENT, hid, sizeof(hid))) {
            ready_ = false;
            return false;
        }
        if (KBD_DEBUG_RAW) {
            ESP_LOGI(TAG, "raw hid mod=0x%02X key=0x%02X", hid[0], hid[1]);
        }
        handle_hid(hid[0], hid[1]);
    }
    return true;
}

const char *Tab5Keyboard::state_label() const
{
    return ready_ ? "KBD READY" : "KBD RETRY";
}
#endif

void Tab5Keyboard::handle_hid(uint8_t modifier, uint8_t keycode)
{
    // A legitimate hold ends with a release event (which differs and resets
    // the counter), so only a truly stuck key repeats this long. 150 events
    // (~5-10s at the A164's repeat rate) keeps long deliberate holds intact
    // while still stopping the endless floods seen live.
    static constexpr uint16_t kStuckRepeatLimit = 150;
    if (modifier == last_event_mod_ && keycode == last_event_key_) {
        if (repeat_count_ < 0xFFFF) {
            ++repeat_count_;
        }
        if (repeat_count_ >= kStuckRepeatLimit) {
            if (!repeat_suppressed_) {
                repeat_suppressed_ = true;
#ifndef TAB5_KEYBOARD_HOST_TEST
                ESP_LOGW(TAG, "suppressing stuck key mod=0x%02X key=0x%02X after %u repeats",
                         modifier, keycode, static_cast<unsigned>(repeat_count_));
#endif
            }
            return;
        }
    } else {
        last_event_mod_ = modifier;
        last_event_key_ = keycode;
        repeat_count_ = 0;
        repeat_suppressed_ = false;
    }

    uint8_t out[8];
    size_t len = translate_hid(modifier, keycode, out, sizeof(out));
    if (len > 0) {
#ifndef TAB5_KEYBOARD_HOST_TEST
        ESP_LOGI(TAG, "key mod=0x%02X key=0x%02X -> queued %u byte(s)",
                 modifier, keycode, static_cast<unsigned>(len));
#endif
        emit_bytes(out, len);
    }
#ifndef TAB5_KEYBOARD_HOST_TEST
    else if (modifier != 0 || keycode != 0) {
        ESP_LOGD(TAG, "unmapped HID key: mod=0x%02X key=0x%02X", modifier, keycode);
    }
#endif
}

void Tab5Keyboard::emit_bytes(const uint8_t *data, size_t len)
{
    if (input_cb_ && data && len > 0) {
        input_cb_(data, len, input_ctx_);
    }
}
