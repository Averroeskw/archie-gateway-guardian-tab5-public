#include "wifi_manager.hpp"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nvs.h"

#include <cstring>

static const char *TAG = "wifi_mgr";
// Last SSID handed to wifi_connect_sta — keys the cached-lease fallback.
static char s_last_ssid[33] = {};
static constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
static constexpr TickType_t WIFI_WAIT_SLICE_TICKS = pdMS_TO_TICKS(500);
static constexpr TickType_t WIFI_PROGRESS_LOG_TICKS = pdMS_TO_TICKS(5000);

static EventGroupHandle_t s_wifi_events = nullptr;
static esp_netif_t *s_sta_netif = nullptr;
static bool s_wifi_initialized = false;
static bool s_handlers_registered = false;
static bool s_wifi_started = false;

static bool sta_has_ip()
{
    if (!s_sta_netif || !s_wifi_events) {
        return false;
    }
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) {
        return false;
    }
    if (ip_info.ip.addr == 0) {
        return false;
    }
    ESP_LOGI(TAG, "Wi-Fi address confirmed by poll");
    xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    return true;
}

// Appliance-grade DNS: router/ISP resolvers with filtering commonly fail on
// public hostnames (observed: getaddrinfo 202 on *.ts.net). lwip does NOT
// fall through on a failed answer — only on timeouts — so the DHCP-provided
// server must be replaced. Re-applied on every (re)connect.
static void apply_dns_override()
{
    if (s_sta_netif == nullptr) {
        return;
    }
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);  // Cloudflare
    esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);  // Google
    esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns);
    ESP_LOGI(TAG, "DNS overridden to public resolvers (1.1.1.1 / 8.8.8.8)");
}

// Lease cache: this AP's DHCP is flaky toward the C6 (assoc solid, OFFERs
// lost). Remember every good lease per-SSID; if DHCP stays silent through
// all the kicks, self-assign the cached lease — the appliance pattern.
static void save_lease_cache(const esp_netif_ip_info_t *info)
{
    nvs_handle_t h = 0;
    if (nvs_open("wifimgr", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "lease_ssid", s_last_ssid);
    nvs_set_u32(h, "lease_ip", info->ip.addr);
    nvs_set_u32(h, "lease_gw", info->gw.addr);
    nvs_set_u32(h, "lease_mask", info->netmask.addr);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t wifi_seed_lease_cache(const char *ssid, const char *ip, const char *gw,
                                const char *netmask)
{
    if (ssid == nullptr || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_netif_ip_info_t info = {};
    info.ip.addr = esp_ip4addr_aton(ip);
    info.gw.addr = esp_ip4addr_aton(gw);
    info.netmask.addr = esp_ip4addr_aton(netmask);
    if (info.ip.addr == 0 || info.gw.addr == 0 || info.netmask.addr == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h = 0;
    ESP_RETURN_ON_ERROR(nvs_open("wifimgr", NVS_READWRITE, &h), TAG, "nvs");
    nvs_set_str(h, "lease_ssid", ssid);
    nvs_set_u32(h, "lease_ip", info.ip.addr);
    nvs_set_u32(h, "lease_gw", info.gw.addr);
    nvs_set_u32(h, "lease_mask", info.netmask.addr);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool apply_cached_lease()
{
    nvs_handle_t h = 0;
    if (nvs_open("wifimgr", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    char ssid[33] = {};
    size_t len = sizeof(ssid);
    esp_netif_ip_info_t info = {};
    bool ok = nvs_get_str(h, "lease_ssid", ssid, &len) == ESP_OK &&
              nvs_get_u32(h, "lease_ip", &info.ip.addr) == ESP_OK &&
              nvs_get_u32(h, "lease_gw", &info.gw.addr) == ESP_OK &&
              nvs_get_u32(h, "lease_mask", &info.netmask.addr) == ESP_OK;
    nvs_close(h);
    if (!ok || info.ip.addr == 0 || std::strcmp(ssid, s_last_ssid) != 0) {
        return false;
    }
    esp_netif_dhcpc_stop(s_sta_netif);
    if (esp_netif_set_ip_info(s_sta_netif, &info) != ESP_OK) {
        esp_netif_dhcpc_start(s_sta_netif);
        return false;
    }
    ESP_LOGW(TAG, "DHCP silent; using cached lease");
    apply_dns_override();
    return true;
}

static void wifi_event_handler(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Wi-Fi STA started");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *disc = static_cast<wifi_event_sta_disconnected_t *>(data);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reason=%d; retrying", disc ? disc->reason : -1);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(TAG, "Wi-Fi connected; address assigned");
        apply_dns_override();
        save_lease_cache(&event->ip_info);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

// Serializes concurrent callers (boot flow, setup wizard, Improv serial): two
// simultaneous connect attempts would fight over esp_wifi state.
static SemaphoreHandle_t s_connect_mutex = nullptr;

namespace {
struct ConnectGuard {
    explicit ConnectGuard(SemaphoreHandle_t m) : mutex(m) { xSemaphoreTake(mutex, portMAX_DELAY); }
    ~ConnectGuard() { xSemaphoreGive(mutex); }
    SemaphoreHandle_t mutex;
};
}  // namespace

// Shared bring-up used by both connect and pre-provisioning scans. Assumes
// the connect mutex is held (or single-threaded boot).
static esp_err_t ensure_started_locked()
{
    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        if (!s_wifi_events) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (!s_sta_netif) {
            return ESP_FAIL;
        }
    }

    if (!s_wifi_initialized) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_initialized = true;
    }

    if (!s_handlers_registered) {
        esp_err_t err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "WIFI_EVENT handler register failed: %s", esp_err_to_name(err));
            return err;
        }

        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "IP_EVENT handler register failed: %s", esp_err_to_name(err));
            return err;
        }
        s_handlers_registered = true;
    }
    return ESP_OK;
}

static esp_err_t take_connect_mutex()
{
    if (!s_connect_mutex) {
        // Racing first calls is not a real scenario (boot creates it long
        // before the wizard or Improv can run), so plain lazy init is fine.
        s_connect_mutex = xSemaphoreCreateMutex();
        if (!s_connect_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t wifi_ensure_started()
{
    ESP_RETURN_ON_ERROR(take_connect_mutex(), TAG, "mutex");
    ConnectGuard guard(s_connect_mutex);
    esp_err_t err = ensure_started_locked();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
        s_wifi_started = true;
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    return ESP_OK;
}

esp_err_t wifi_connect_sta(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(take_connect_mutex(), TAG, "mutex");
    ConnectGuard guard(s_connect_mutex);

    esp_err_t init_err = ensure_started_locked();
    if (init_err != ESP_OK) {
        return init_err;
    }

    wifi_config_t wifi_config = {};
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.ssid), ssid, sizeof(wifi_config.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.password), password ? password : "", sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    strlcpy(s_last_ssid, ssid, sizeof(s_last_ssid));
    // Never print network identifiers: logs are commonly attached to bug reports.
    ESP_LOGI(TAG, "Connecting to configured Wi-Fi network");
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_started = true;
        // Modem sleep on the C6 link adds 100-300ms keystroke latency spikes;
        // an interactive terminal wants the radio awake.
        err = esp_wifi_set_ps(WIFI_PS_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_ps(NONE) failed: %s", esp_err_to_name(err));
        }
    } else {
        esp_wifi_disconnect();
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    TickType_t last_log = start;
    EventBits_t bits = 0;
    TickType_t assoc_at = 0;   // first tick we observed association
    int dhcp_kicks = 0;

    // Wrap-safe: compare elapsed ticks, not absolute counter values, because the
    // FreeRTOS tick counter (an unsigned TickType_t) periodically overflows.
    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        bits = xEventGroupWaitBits(
            s_wifi_events,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdFALSE,
            WIFI_WAIT_SLICE_TICKS);
        if (bits & WIFI_CONNECTED_BIT) {
            break;
        }
        if (sta_has_ip()) {
            bits |= WIFI_CONNECTED_BIT;
            break;
        }
        TickType_t now = xTaskGetTickCount();
        if ((now - last_log) >= WIFI_PROGRESS_LOG_TICKS) {
            // Show what we're actually associated to while DHCP is pending —
            // distinguishes "wrong band/AP" from "lease never granted".
            wifi_ap_record_t ap = {};
            bool associated = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
            if (associated) {
                ESP_LOGI(TAG, "Waiting for IP... associated chan=%d rssi=%d auth=%d",
                         ap.primary, ap.rssi, ap.authmode);
                if (assoc_at == 0) {
                    assoc_at = now;
                }
            } else {
                ESP_LOGI(TAG, "Waiting for Wi-Fi IP... (not associated)");
                assoc_at = 0;
            }
            // The esp-hosted C6 path drops DHCP OFFERs with some APs. Kick
            // the client 6s after association settles, then every ~10s (up
            // to 3 times) — measured against ASSOCIATION time, not call
            // time, so a slow association doesn't waste the kicks.
            if (associated && dhcp_kicks < 3 &&
                (now - assoc_at) >= pdMS_TO_TICKS(4000 + dhcp_kicks * 6000)) {
                ++dhcp_kicks;
                ESP_LOGW(TAG, "no DHCP lease yet; restarting DHCP client (attempt %d/3)", dhcp_kicks);
                esp_netif_dhcpc_stop(s_sta_netif);
                esp_netif_dhcpc_start(s_sta_netif);
            } else if (associated && dhcp_kicks == 3 &&
                       (now - assoc_at) >= pdMS_TO_TICKS(21000)) {
                dhcp_kicks = 4;  // one shot
                apply_cached_lease();
            }
            last_log = now;
        }
    }

    if (!(bits & WIFI_CONNECTED_BIT)) {
        sta_has_ip();
        bits = xEventGroupGetBits(s_wifi_events);
    }

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}
