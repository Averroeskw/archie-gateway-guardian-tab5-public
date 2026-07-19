#pragma once

#include "profile_store.hpp"

#include "esp_err.h"

#include <cstddef>
#include <cstdint>

// Mode runner for the Hermes command centre. Owns the console screen,
// settings screen, WebSocket client and telemetry, and wires them together.
// Wi-Fi should normally be connected first, but a failed Wi-Fi is survivable:
// the WS client auto-reconnects once the link comes up, and the SETTINGS
// screen stays reachable for fixing credentials. Never returns control of
// the display: the console stays loaded until reboot.
//
// `store`, `wifi` and `hermes` must outlive the mode (app_main statics): the
// settings screen edits them in place and persists via the store.
esp_err_t hermes_mode_start(ProfileStore *store, WifiProfile *wifi, HermesProfile *hermes);

// True once hermes_mode_start has taken over the screen; app_main's
// keyboard task routes raw key bytes here.
bool hermes_mode_active();
void hermes_mode_key_input(const uint8_t *data, size_t len);
