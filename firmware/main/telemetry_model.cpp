#include "telemetry_model.hpp"

#include "bsp/m5stack_tab5.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <algorithm>

static const char *TAG = "telemetry";

// INA226 power monitor on the BSP I2C bus. Bus-voltage register LSB is
// 1.25 mV; the Tab5 battery kit is a 2S pack, mapped linearly 6.0 V -> 0%,
// 8.4 V -> 100%. There is no fuel gauge on this board.
static constexpr uint8_t kIna226Addr = 0x41;
static constexpr uint8_t kIna226BusVoltageReg = 0x02;
static constexpr float kBatteryEmptyVolts = 6.0f;
static constexpr float kBatteryFullVolts = 8.4f;

#define LOCK() xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_))

void TelemetryModel::begin()
{
    mutex_ = xSemaphoreCreateMutex();
    configASSERT(mutex_);
}

void TelemetryModel::set_wifi(bool connected, int rssi_dbm, const char *ip)
{
    LOCK();
    state_.wifi_connected = connected;
    state_.wifi_rssi_dbm = rssi_dbm;
    state_.ip_address = ip != nullptr ? ip : "";
    UNLOCK();
}

void TelemetryModel::set_gateway(bool connected, const char *host)
{
    LOCK();
    state_.gateway_connected = connected;
    if (host != nullptr) {
        state_.gateway_host = host;
    }
    UNLOCK();
}

void TelemetryModel::add_ws_traffic(uint32_t rx_bytes, uint32_t tx_bytes)
{
    LOCK();
    state_.ws_rx_bytes += rx_bytes;
    state_.ws_tx_bytes += tx_bytes;
    UNLOCK();
}

void TelemetryModel::set_agent(const char *persona, const char *state)
{
    LOCK();
    if (persona != nullptr && persona[0] != '\0') {
        state_.persona = persona;
    }
    if (state != nullptr && state[0] != '\0') {
        state_.agent_state = state;
    }
    UNLOCK();
}

void TelemetryModel::set_tokens(uint32_t tokens_in, uint32_t tokens_out)
{
    LOCK();
    state_.tokens_in = tokens_in;
    state_.tokens_out = tokens_out;
    UNLOCK();
}

void TelemetryModel::set_ptt(bool active, uint16_t mic_level)
{
    LOCK();
    state_.ptt_active = active;
    state_.mic_level = mic_level;
    UNLOCK();
}

int TelemetryModel::read_battery_percent(float &volts_out)
{
    if (ina226_failed_) {
        return -1;
    }
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == nullptr) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            ESP_LOGW(TAG, "BSP I2C bus not available; battery gauge disabled");
        }
        return -1;
    }
    // One-shot bus survey: log every ACKing address so a missing/misplaced
    // power monitor is diagnosable from serial instead of a silent "USB".
    static bool scanned = false;
    if (!scanned) {
        scanned = true;
        char found[128] = {};
        size_t off = 0;
        for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
            if (i2c_master_probe(bus, addr, 20) == ESP_OK && off < sizeof(found) - 6) {
                off += snprintf(found + off, sizeof(found) - off, "0x%02x ", addr);
            }
        }
        ESP_LOGI(TAG, "I2C devices: %s", off ? found : "(none)");
    }
    static i2c_master_dev_handle_t s_dev = nullptr;
    if (s_dev == nullptr) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = kIna226Addr,
            .scl_speed_hz = 100000,
            .scl_wait_us = 0,
            .flags = {},
        };
        if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
            ESP_LOGW(TAG, "INA226 add-device failed; battery gauge disabled");
            ina226_failed_ = true;
            return -1;
        }
    }
    uint8_t reg = kIna226BusVoltageReg;
    uint8_t raw[2] = {};
    esp_err_t rerr = i2c_master_transmit_receive(s_dev, &reg, 1, raw, 2, 50);
    static bool first_read_logged = false;
    if (rerr != ESP_OK) {
        if (!first_read_logged) {
            first_read_logged = true;
            ESP_LOGW(TAG, "INA226 first read failed: %s", esp_err_to_name(rerr));
        }
        return -1;
    }
    uint16_t counts = static_cast<uint16_t>((raw[0] << 8) | raw[1]);
    float volts = counts * 0.00125f;
    if (!first_read_logged) {
        first_read_logged = true;
        ESP_LOGI(TAG, "INA226 first read OK: %.2fV", static_cast<double>(volts));
    }
    volts_out = volts;
    float pct = (volts - kBatteryEmptyVolts) / (kBatteryFullVolts - kBatteryEmptyVolts) * 100.0f;
    return std::clamp(static_cast<int>(pct), 0, 100);
}

void TelemetryModel::sample_device()
{
    float volts = 0.0f;
    int pct = read_battery_percent(volts);
    uint32_t heap_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    uint32_t psram_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    uint32_t uptime = xTaskGetTickCount() * portTICK_PERIOD_MS / 1000;
    LOCK();
    state_.battery_percent = pct;
    state_.battery_volts = volts;
    state_.free_heap_kb = heap_kb;
    state_.free_psram_kb = psram_kb;
    state_.uptime_sec = uptime;
    UNLOCK();
}

TelemetrySnapshot TelemetryModel::snapshot() const
{
    LOCK();
    TelemetrySnapshot copy = state_;
    UNLOCK();
    return copy;
}
