#pragma once

#include <cstddef>
#include <cstdint>

class Tab5Keyboard {
public:
    using InputCallback = void (*)(const uint8_t *data, size_t len, void *ctx);

    bool begin();
    bool poll();
    bool set_rgb(uint8_t red, uint8_t green, uint8_t blue);
    bool ready() const { return ready_; }
    const char *state_label() const;
    void set_input_callback(InputCallback cb, void *ctx)
    {
        input_cb_ = cb;
        input_ctx_ = ctx;
    }

    static size_t translate_hid(uint8_t modifier, uint8_t keycode, uint8_t *out, size_t out_cap);

private:
    bool write_reg(uint8_t reg, uint8_t value);
    bool write_bytes(uint8_t reg, const uint8_t *data, size_t len);
    bool read_reg(uint8_t reg, uint8_t *data, size_t len);
    void handle_hid(uint8_t modifier, uint8_t keycode);
    void emit_bytes(const uint8_t *data, size_t len);

    InputCallback input_cb_ = nullptr;
    void *input_ctx_ = nullptr;
    bool ready_ = false;
    bool warned_offline_ = false;
    uint32_t last_rgb_ms_ = 0;

    // Stuck-key guard: the A164 auto-repeats by resending the same HID event;
    // a flooded/stuck key (seen live with 0x2A) repeats it without end.
    uint8_t last_event_mod_ = 0xFF;
    uint8_t last_event_key_ = 0xFF;
    uint16_t repeat_count_ = 0;
    bool repeat_suppressed_ = false;

    // Periodic HID-mode re-assert to recover a silently-demoted keyboard MCU.
    static constexpr uint32_t kMaintainIntervalMs = 5000;
    uint32_t last_maintain_ms_ = 0;
};
