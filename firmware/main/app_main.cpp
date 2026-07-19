#include "app_config.hpp"
#include "hermes_mode.hpp"
#include "hermes_theme.hpp"
#include "improv_serial.hpp"
#include "keyboard_status.hpp"
#include "profile_import.hpp"
#include "profile_store.hpp"
#include "setup_wizard.hpp"
#include "splash_screen.hpp"
#include "tab5_keyboard.hpp"
#include "wifi_manager.hpp"

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>

static const char *TAG = "app_main";

static ProfileStore s_profiles;
static WifiProfile s_wifi;
static HermesProfile s_hermes;
static Tab5Keyboard s_hw_keyboard;
static SplashScreen s_splash;
static bool s_wifi_override = false;
static TickType_t s_splash_shown_at = 0;

// Init often outpaces the eye: hold the Nexus bootstrap long enough for its
// four staged system locks and final operational state to be legible.
static constexpr uint32_t kSplashMinMs = 3000;

static void wait_splash_minimum(void)
{
    TickType_t elapsed = xTaskGetTickCount() - s_splash_shown_at;
    uint32_t elapsed_ms = elapsed * portTICK_PERIOD_MS;
    if (elapsed_ms < kSplashMinMs) {
        vTaskDelay(pdMS_TO_TICKS(kSplashMinMs - elapsed_ms));
    }
}

// ---- display ---------------------------------------------------------------

// Match the proven fave-build-2 Archie pipeline: two full panel buffers in
// PSRAM with software rotation. This removes the visibly chunked 20-line
// strip flush and avoids the ESP32-P4 PPA SRM erratum present in IDF 5.4.2.
// Animated surfaces remain bounded, so normal refreshes still use dirty areas.
static esp_err_t start_display(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
        .double_buffer = true,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = true,
        },
    };
    lv_display_t *display = bsp_display_start_with_config(&cfg);
    if (display == nullptr) {
        return ESP_FAIL;
    }
    // The panel is physically 720x1280 portrait; the whole UI is laid out
    // 1280x720 landscape. sw_rotate above only enables the capability —
    // this call actually applies it (and keeps touch mapping in sync).
    // 0 = wait forever in esp_lvgl_port: a skipped rotation would mean a
    // silently sideways UI, so blocking here is strictly better.
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "start_display: display lock unavailable");
        return ESP_FAIL;
    }
    bsp_display_rotate(display, LV_DISPLAY_ROTATION_90);
    bsp_display_unlock();
    return ESP_OK;
}

static void reveal_backlight(void)
{
    // Give LVGL one refresh while the panel is dark so the first visible scan
    // is the complete cosmic surface rather than the panel's power-on white.
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_display_brightness_set(100));
}

// ---- keyboard --------------------------------------------------------------

// Raw key bytes route to whichever surface owns the screen: the wizard during
// first-run setup, otherwise the console.
static void physical_keyboard_input_cb(const uint8_t *data, size_t len, void *)
{
    if (setup_wizard_active()) {
        if (bsp_display_lock(0)) {
            setup_wizard_key_input(data, len);
            bsp_display_unlock();
        }
        return;
    }
    if (hermes_mode_active()) {
        hermes_mode_key_input(data, len);
    }
}

static void physical_keyboard_task(void *)
{
    s_hw_keyboard.set_input_callback(physical_keyboard_input_cb, nullptr);
    for (;;) {
        if (!s_hw_keyboard.ready()) {
            if (!s_hw_keyboard.begin()) {
                keyboard_status_set(false);
                vTaskDelay(pdMS_TO_TICKS(600));
                continue;
            }
        }
        if (!s_hw_keyboard.poll()) {
            keyboard_status_set(false);
            vTaskDelay(pdMS_TO_TICKS(600));
            continue;
        }
        keyboard_status_set(true);
        vTaskDelay(pdMS_TO_TICKS(12));
    }
}

// ---- SD profile (optional fleet provisioning) ------------------------------

static void import_sd_profile(void)
{
    if (bsp_sdcard_mount() != ESP_OK) {
        ESP_LOGI(TAG, "no SD card / mount failed; using NVS profile only");
        return;
    }
    bool changed = false;
    esp_err_t err = import_profile_file(PROFILE_IMPORT_PATH, s_wifi, s_hermes, changed, s_wifi_override);
    if (err == ESP_OK && changed) {
        ESP_LOGI(TAG, "imported %s from SD; persisting", PROFILE_IMPORT_PATH);
        s_profiles.save(s_wifi, s_hermes);
    }
    bsp_sdcard_unmount();
}

// ---- Improv Wi-Fi (serial provisioning from ESP Web Tools etc.) ------------

// Runs on the improv task. Persist first so the credentials survive a reboot
// even if this connect attempt times out.
static bool improv_on_credentials(const char *ssid, const char *password, void *)
{
    s_wifi.ssid = ssid;
    s_wifi.password = password != nullptr ? password : "";
    s_profiles.save(s_wifi, s_hermes);
    s_profiles.save_wifi_override(true);
    return wifi_connect_sta(ssid, password, 50000) == ESP_OK;
}

static bool improv_is_provisioned(void *)
{
    wifi_ap_record_t ap = {};
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

// ---- console entry ---------------------------------------------------------

static void enter_console(void)
{
    if (hermes_mode_start(&s_profiles, &s_wifi, &s_hermes) != ESP_OK) {
        ESP_LOGE(TAG, "console failed to start");
    }
    // The console loads with auto_del: LVGL deletes whichever screen was up
    // (splash or the wizard's test step) during the immediate swap. Drop our
    // splash pointers so nothing touches it again.
    s_splash.forget();
}

// Called by the wizard once the link self-test passes. Wi-Fi is already up
// (the wizard connected it during the test), so go straight to the console.
static void wizard_done(bool ok, void *)
{
    if (ok) {
        enter_console();
    }
}

extern "C" void app_main(void)
{
    // NVS is initialized by ProfileStore::begin(); just bring up netif + the
    // default event loop that the Wi-Fi stack needs.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(start_display());
    ESP_ERROR_CHECK(bsp_display_brightness_set(0));

    if (bsp_display_lock(0)) {
        hermes_theme::init();
        s_splash.show("starting…");
        bsp_display_unlock();
    }
    s_splash_shown_at = xTaskGetTickCount();
    reveal_backlight();

    ESP_ERROR_CHECK(s_profiles.begin());
    s_profiles.load_wifi_override(s_wifi_override);
    s_profiles.load(s_wifi, s_hermes);

    if (bsp_display_lock(0)) {
        s_splash.set_status("reading profile…");
        bsp_display_unlock();
    }
    import_sd_profile();

    xTaskCreatePinnedToCore(physical_keyboard_task, "tab5_keyboard", 4096, nullptr, 5, nullptr, 0);

    // Bring up the C6 Wi-Fi radio and let it settle before any STA connect.
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_WIFI, true));
    vTaskDelay(pdMS_TO_TICKS(500));

    // Enable battery charging: IP2326 CHG_EN sits on IO expander 1 pin 7
    // (M5Unified's Tab5 init drives it; the BSP does not). Without it the
    // pack never charges and eventually drains to protection cutoff — the
    // INA226 then reads ~1V and the gauge shows empty forever.
    {
        esp_io_expander_handle_t exp1 = bsp_io_expander1_init();
        if (exp1 != nullptr) {
            esp_io_expander_set_dir(exp1, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
            esp_io_expander_set_output_mode(exp1, IO_EXPANDER_PIN_NUM_7,
                                            IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
            esp_io_expander_set_level(exp1, IO_EXPANDER_PIN_NUM_7, 1);
            ESP_LOGI(TAG, "battery charging enabled (CHG_EN high)");
        } else {
            ESP_LOGW(TAG, "IO expander 1 unavailable; charging not enabled");
        }
    }

    // Start SNTP now (it retries on its own until the network is up): TLS
    // certificate validation needs a sane clock, and the wizard's wss://
    // connection test can run long before the console would have synced.
    // Default zone is UTC; set a POSIX TZ string for local time.
    setenv("TZ", "UTC0", 1);
    tzset();
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp_cfg.start = true;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_sntp_init(&sntp_cfg));

    // Improv Wi-Fi over the USB port: browsers (ESP Web Tools) can provision
    // Wi-Fi right after flashing, no on-device typing. Best-effort.
    if (improv_serial_start(improv_on_credentials, improv_is_provisioned, nullptr) != ESP_OK) {
        ESP_LOGW(TAG, "Improv serial unavailable; wizard/settings still work");
    }

    // Presence-only diagnostics are safe to share in bug reports: never log
    // SSIDs, endpoints, tokens, passwords, or API keys.
    ESP_LOGI(TAG, "profile: wifi=%s gateway=%s token=%s mode=%s boot=%s",
             s_wifi.ssid.empty() ? "missing" : "set",
             s_hermes.gateway_ws_url.empty() ? "missing" : "set",
             s_hermes.gateway_token.empty() ? "missing" : "set",
             s_hermes.connection_mode.c_str(), s_hermes.boot_mode.c_str());

    bool configured = profile_is_configured(s_wifi, s_hermes) && s_hermes.boot_mode != "setup";

    if (!configured) {
        ESP_LOGI(TAG, "unconfigured (or boot_mode=setup): launching setup wizard");
        wait_splash_minimum();
        if (bsp_display_lock(0)) {
            // The wizard swaps in with auto_del, so LVGL owns deletion of the
            // splash and its animations.
            setup_wizard_start(&s_profiles, &s_wifi, &s_hermes, wizard_done, nullptr);
            s_splash.forget();
            bsp_display_unlock();
        }
        return;  // the wizard drives the rest; wizard_done() enters the console
    }

    // Configured: enter the console immediately and bring Wi-Fi up in the
    // background — on a slow AP the old blocking connect held the splash for
    // tens of seconds. The console's chips/state show the link coming alive,
    // and the WS client reconnects on its own once an IP lands.
    wait_splash_minimum();
    xTaskCreate([](void *) {
        esp_err_t werr = wifi_connect_sta(s_wifi.ssid.c_str(), s_wifi.password.c_str(), 50000);
        if (werr != ESP_OK) {
            ESP_LOGW(TAG, "background Wi-Fi connect failed (%s); ws client keeps retrying",
                     esp_err_to_name(werr));
        }
        vTaskDelete(nullptr);
    }, "wifi_boot", 4096, nullptr, 3, nullptr);
    enter_console();
}
