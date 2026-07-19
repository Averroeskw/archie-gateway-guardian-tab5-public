#pragma once

#include "esp_err.h"

#include <cstdint>

esp_err_t wifi_connect_sta(const char *ssid, const char *password, uint32_t timeout_ms);

// Bring the STA interface up (init + start, no connect) so scans work before
// any credentials exist. Idempotent; wifi_connect_sta covers this itself.
esp_err_t wifi_ensure_started();

// Seed the per-SSID lease cache (dotted-quad strings) — used by the Improv
// vendor RPC for networks whose DHCP will not serve this device. The cached
// lease is applied only after DHCP stays silent through every retry.
esp_err_t wifi_seed_lease_cache(const char *ssid, const char *ip, const char *gw,
                                const char *netmask);
