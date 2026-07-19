#pragma once

#include <cstdint>
#include <string>

// Shared device + session telemetry behind one mutex. Producers run on their
// own tasks (Wi-Fi events, WS client, audio task, 1Hz sampler); the console
// screen consumes a snapshot from the LVGL task and repaints only the widgets
// whose values changed.
struct TelemetrySnapshot {
    // Device
    int wifi_rssi_dbm = 0;
    bool wifi_connected = false;
    std::string ip_address;
    int battery_percent = -1;  // -1 = unknown (INA226 not readable)
    float battery_volts = 0.0f;
    uint32_t free_heap_kb = 0;
    uint32_t free_psram_kb = 0;
    uint32_t uptime_sec = 0;

    // Gateway link
    bool gateway_connected = false;
    std::string gateway_host;
    uint32_t ws_rx_bytes = 0;
    uint32_t ws_tx_bytes = 0;

    // Agent session (fed by gateway status envelopes)
    std::string persona = "hermes";
    std::string agent_state = "idle";  // idle | thinking | speaking | error
    uint32_t tokens_in = 0;
    uint32_t tokens_out = 0;

    // Audio
    bool ptt_active = false;
    uint16_t mic_level = 0;  // 0..1000 RMS scale for the waveform widget
};

class TelemetryModel {
public:
    void begin();

    // Producers (any task).
    void set_wifi(bool connected, int rssi_dbm, const char *ip);
    void set_gateway(bool connected, const char *host);
    void add_ws_traffic(uint32_t rx_bytes, uint32_t tx_bytes);
    void set_agent(const char *persona, const char *state);
    void set_tokens(uint32_t tokens_in, uint32_t tokens_out);
    void set_ptt(bool active, uint16_t mic_level);

    // 1Hz sampler: refreshes heap/PSRAM/uptime/battery in place. Call from a
    // low-priority task, not the LVGL task (the INA226 read does I2C I/O).
    void sample_device();

    // Consumer (LVGL task).
    TelemetrySnapshot snapshot() const;

private:
    int read_battery_percent(float &volts_out);

    mutable void *mutex_ = nullptr;  // SemaphoreHandle_t, kept opaque here
    TelemetrySnapshot state_;
    bool ina226_failed_ = false;
};
