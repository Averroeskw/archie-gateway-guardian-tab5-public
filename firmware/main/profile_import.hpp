#pragma once

#include "esp_err.h"
#include "profile_store.hpp"

// skip_wifi: ignore wifi_ssid/wifi_password from the file — set when the
// user has saved Wi-Fi credentials from the on-device settings screen
// (ProfileStore wifi-override flag), so SD re-import can't revert them.
esp_err_t import_profile_file(const char *path, WifiProfile &wifi, HermesProfile &hermes,
                              bool &changed, bool skip_wifi = false);
