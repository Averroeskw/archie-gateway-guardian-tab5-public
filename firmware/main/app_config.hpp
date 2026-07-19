#pragma once

#include <cstdint>

// Private developer overrides live in an ignored header. Public and CI builds
// do not have this file and always compile the credential-free defaults below.
#if __has_include("archie_gateway.local.h")
#include "archie_gateway.local.h"
#endif

// M5Stack Tab5 panel geometry (landscape).
static constexpr int LCD_W = 1280;
static constexpr int LCD_H = 720;
static constexpr int STATUS_H = 36;

#ifdef LOCAL_NVS_NAMESPACE
// ESP-IDF NVS namespace names are limited to 15 characters (plus NUL).
static_assert(sizeof(LOCAL_NVS_NAMESPACE) <= 16,
              "LOCAL_NVS_NAMESPACE must be 15 characters or fewer");
static constexpr const char *APP_NAMESPACE = LOCAL_NVS_NAMESPACE;
#else
static constexpr const char *APP_NAMESPACE = "archieos";
#endif
// Optional SD-card profile for fleet provisioning (see docs/QUICKSTART.md).
static constexpr const char *PROFILE_IMPORT_PATH = "/sdcard/archie_profile.conf";

// Idle seconds before the backlight dims (privacy + panel life).
static constexpr int SCREEN_DIM_TIMEOUT_SEC = 120;
static constexpr int SCREEN_DIM_BRIGHTNESS = 15;

// Gateway defaults. Ship empty: the first-run wizard collects the real
// values and stores them in NVS. Point your own gateway at something like
//   wss://your-server.example.com/ws/tab5
//
// Development builds can pre-fill BOTH without ever entering version control:
// create main/archie_gateway.local.h (gitignored via *.local.h) containing
//   #define LOCAL_GATEWAY_TOKEN  "<your gateway token>"
//   #define LOCAL_GATEWAY_WS_URL "ws://<dev-host>:8787/ws/tab5"   // optional
//   #define LOCAL_NVS_NAMESPACE  "<legacy namespace>"              // optional
// The wizard shows these pre-filled; the user just confirms.
#ifdef LOCAL_GATEWAY_WS_URL
static constexpr const char *DEFAULT_GATEWAY_WS_URL = LOCAL_GATEWAY_WS_URL;
#else
static constexpr const char *DEFAULT_GATEWAY_WS_URL = "";
#endif
#ifdef LOCAL_GATEWAY_TOKEN
static constexpr const char *DEFAULT_GATEWAY_TOKEN = LOCAL_GATEWAY_TOKEN;
#else
static constexpr const char *DEFAULT_GATEWAY_TOKEN = "";
#endif
static constexpr const char *DEFAULT_PERSONA = "archie";

// Reported in the gateway hello envelope and the SYSTEM panel.
static constexpr const char *ARCHIE_OS_VERSION = "0.1.0-alpha";

// Log raw HID events + pending-event counts from the keyboard poll loop.
static constexpr bool KBD_DEBUG_RAW = false;
