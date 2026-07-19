#pragma once

#include "profile_store.hpp"
#include "ws_hermes_client.hpp"

#include "esp_err.h"

#include <cstddef>
#include <cstdint>

// First-run setup wizard: walks the user from a blank device to a working
// link. Shown when the profile is incomplete or boot_mode == "setup". Steps:
//   1. Wi-Fi       — SSID + password
//   2. Link        — Hermes / OpenClaw / OpenAI / Claude / custom
//   3. Credentials — URL + token/key + model
//   4. Voice       — optional ElevenLabs key + voice ID
//   5. Test        — live Wi-Fi and provider/gateway verification
//
// On success it saves the profile and calls `done(true)`; the caller then
// enters the console. `done(false)` is never sent — the wizard loops on the
// test screen until the link is healthy or the user edits and retries.
//
// All calls run on the LVGL task under bsp_display_lock(), like the other
// screens. `store`, `wifi` and `hermes` must outlive the wizard.
using SetupDoneCallback = void (*)(bool ok, void *ctx);

void setup_wizard_start(ProfileStore *store, WifiProfile *wifi, HermesProfile *hermes,
                        SetupDoneCallback done, void *ctx);

// True while the wizard owns the screen; app_main routes keyboard bytes here.
bool setup_wizard_active();
void setup_wizard_key_input(const uint8_t *data, size_t len);
