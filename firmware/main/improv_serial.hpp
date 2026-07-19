#pragma once

#include "esp_err.h"

#include <cstddef>
#include <cstdint>

// Improv Wi-Fi over Serial (https://www.improv-wifi.com/serial/): lets tools
// like ESP Web Tools provision Wi-Fi from the browser right after flashing —
// no on-device typing. Runs as a small task reading the USB-Serial-JTAG port
// (the Tab5's USB-C). Log output may interleave with Improv frames; clients
// scan for the "IMPROV" header, so that is fine per the spec.
//
// The callback receives credentials and must BLOCK while it tries to connect
// (it runs on the improv task, not the LVGL task). Return true when the
// network is up; the caller reports PROVISIONED / errors to the host.
using ImprovCredentialsCallback = bool (*)(const char *ssid, const char *password, void *ctx);

// True once a Wi-Fi link is already up (reported for GET_CURRENT_STATE).
using ImprovProvisionedQuery = bool (*)(void *ctx);

esp_err_t improv_serial_start(ImprovCredentialsCallback on_credentials,
                              ImprovProvisionedQuery is_provisioned, void *ctx);

// Vendor RPC 0x7A (outside the Improv standard): seed the Wi-Fi manager's
// per-SSID lease cache with a static fallback (ssid, ip, gateway, netmask as
// length-prefixed strings). For networks whose DHCP won't serve the device —
// the lease applies only when DHCP stays silent after all retries.
