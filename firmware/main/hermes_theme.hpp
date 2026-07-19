#pragma once

#include "lvgl.h"

#include <cstdint>

// Mission-console palette: black ground, white text, amber CRT accents,
// yellow reserved for the top infographic bar and OK states.
namespace hermes_theme {

constexpr uint32_t COL_BG = 0x070605;         // black background
constexpr uint32_t COL_PANEL = 0x12100C;      // panel fill
constexpr uint32_t COL_PANEL_EDGE = 0x3A3022; // panel border
constexpr uint32_t COL_TEXT = 0xF2EFE9;       // primary white text
constexpr uint32_t COL_DIM = 0x8C8478;        // secondary text / inactive
constexpr uint32_t COL_SUN = 0xF28A48;        // sun-orange accent
constexpr uint32_t COL_YELLOW = 0xF2C84B;     // top bar / OK / live activity
constexpr uint32_t COL_ALERT = 0xFF5A2A;      // error / STOP (hot orange)
constexpr uint32_t COL_AMBER = 0xFFB000;      // Nexus amber (primary accent)
constexpr uint32_t COL_CYAN = 0x4FD8EB;       // MIRA cyan accent
constexpr uint32_t COL_MIRA = 0xFFFFFF;       // pure white (custom persona)
constexpr uint32_t COL_DECK_BG = 0x0D0B0A;    // DECK console ground
constexpr uint32_t COL_TILE = 0x1A1713;       // DECK tile fill
constexpr uint32_t COL_OK = 0x8CD973;         // healthy/online green

// Persona -> accent color: hermes = amber, archie = sun-orange, mira = cyan,
// anything else (custom personas) = white.
uint32_t persona_accent(const char *persona);

// One-time global style init. Call once before building screens, on the LVGL
// task (under the display lock).
void init();

// Fixed-layout panel with a 1px amber edge, faint glow shadow and an optional
// small uppercase title tag overlapping the top edge. Returns the panel.
lv_obj_t *make_panel(lv_obj_t *parent, const char *title, int x, int y, int w, int h);

// Square-bracket corner accents drawn just inside a panel's four corners.
void add_corner_brackets(lv_obj_t *panel);

// Full-screen scanline overlay (every 4th row darkened). Implemented as one
// tiled 1x4 image object, so each LVGL dirty area recomposites it with a
// plain alpha blit — no per-line objects, no extra invalidations.
lv_obj_t *add_scanlines(lv_obj_t *screen);

// Console action button (bottom deck). Returns the button; label is centered.
lv_obj_t *make_deck_button(lv_obj_t *parent, const char *text, uint32_t accent);

// DECK-style tile: soft rounded card (radius 20, no brackets, no glow) with
// an optional small tracked-caps title inside the top-left. Returns the tile.
lv_obj_t *make_tile(lv_obj_t *parent, const char *title, int x, int y, int w, int h);

const lv_font_t *font_title();
const lv_font_t *font_body();
const lv_font_t *font_mono();
const lv_font_t *font_small();  // chips, tags, secondary rows

}  // namespace hermes_theme
