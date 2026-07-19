#include "hermes_theme.hpp"

#include <cstring>

LV_FONT_DECLARE(hack_mono_18);

namespace hermes_theme {

// 1x4 ARGB8888 column: three transparent rows + one ~12% black row. Tiled
// across the screen it darkens every 4th physical line into a CRT scanline.
static uint8_t s_scanline_px[4 * 4];
static lv_image_dsc_t s_scanline_dsc;
static bool s_inited = false;

void init()
{
    if (s_inited) {
        return;
    }
    std::memset(s_scanline_px, 0, sizeof(s_scanline_px));
    // LVGL ARGB8888 byte order: B, G, R, A.
    s_scanline_px[12] = 0x00;
    s_scanline_px[13] = 0x00;
    s_scanline_px[14] = 0x00;
    s_scanline_px[15] = 0x20;  // last row: black at ~12% alpha

    std::memset(&s_scanline_dsc, 0, sizeof(s_scanline_dsc));
    s_scanline_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_scanline_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    s_scanline_dsc.header.w = 1;
    s_scanline_dsc.header.h = 4;
    s_scanline_dsc.header.stride = 4;
    s_scanline_dsc.data = s_scanline_px;
    s_scanline_dsc.data_size = sizeof(s_scanline_px);
    s_inited = true;
}

lv_obj_t *make_panel(lv_obj_t *parent, const char *title, int x, int y, int w, int h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(COL_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    // Faint orange bloom around each panel stands in for per-glyph glow: a box
    // shadow is one cheap draw task, duplicate glow labels would double text
    // work on a renderer we already had to fight for headroom.
    lv_obj_set_style_shadow_color(panel, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(panel, 16, 0);
    lv_obj_set_style_shadow_spread(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    if (title != nullptr && title[0] != '\0') {
        lv_obj_t *tag = lv_label_create(parent);
        lv_label_set_text(tag, title);
        lv_obj_set_style_text_color(tag, lv_color_hex(COL_YELLOW), 0);
        lv_obj_set_style_text_font(tag, font_mono(), 0);
        lv_obj_set_style_bg_color(tag, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(tag, 6, 0);
        lv_obj_set_pos(tag, x + 14, y - 9);
    }
    return panel;
}

static lv_obj_t *bracket_line(lv_obj_t *panel, const lv_point_precise_t *pts)
{
    lv_obj_t *line = lv_line_create(panel);
    lv_line_set_points(line, pts, 3);
    lv_obj_set_style_line_color(line, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_line_width(line, 2, 0);
    lv_obj_set_style_line_rounded(line, false, 0);
    return line;
}

void add_corner_brackets(lv_obj_t *panel)
{
    static constexpr int kArm = 14;
    int w = lv_obj_get_width(panel);
    int h = lv_obj_get_height(panel);
    if (w <= 0 || h <= 0) {
        // Not laid out yet (fixed layout, so update_layout is cheap here).
        lv_obj_update_layout(panel);
        w = lv_obj_get_width(panel);
        h = lv_obj_get_height(panel);
    }
    // Panel-local coordinates, inset 2px from each corner. Points are stored
    // per-call in static arrays per corner: lv_line keeps the pointer.
    // 4 corners x 3 points; a panel using brackets is created exactly once.
    static lv_point_precise_t pts[4][3];
    const int pad = 2;
    const lv_value_precise_t right = static_cast<lv_value_precise_t>(w - 2 * 8 - pad);
    const lv_value_precise_t bottom = static_cast<lv_value_precise_t>(h - 2 * 8 - pad);
    pts[0][0] = {pad, pad + kArm};
    pts[0][1] = {pad, pad};
    pts[0][2] = {pad + kArm, pad};
    pts[1][0] = {right - kArm, pad};
    pts[1][1] = {right, pad};
    pts[1][2] = {right, pad + kArm};
    pts[2][0] = {right, bottom - kArm};
    pts[2][1] = {right, bottom};
    pts[2][2] = {right - kArm, bottom};
    pts[3][0] = {pad + kArm, bottom};
    pts[3][1] = {pad, bottom};
    pts[3][2] = {pad, bottom - kArm};
    for (auto &corner : pts) {
        bracket_line(panel, corner);
    }
}

lv_obj_t *add_scanlines(lv_obj_t *screen)
{
    init();
    lv_obj_t *overlay = lv_image_create(screen);
    lv_image_set_src(overlay, &s_scanline_dsc);
    lv_obj_set_size(overlay, lv_obj_get_width(screen), lv_obj_get_height(screen));
    lv_image_set_inner_align(overlay, LV_IMAGE_ALIGN_TILE);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    return overlay;
}

lv_obj_t *make_deck_button(lv_obj_t *parent, const char *text, uint32_t accent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_PANEL_EDGE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_width(btn, 2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 4, 0);
    // Accent-tinted glow lifts the deck off the black ground.
    lv_obj_set_style_shadow_color(btn, lv_color_hex(accent), 0);
    lv_obj_set_style_shadow_width(btn, 8, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_20, 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(accent), 0);
    lv_obj_set_style_text_font(label, font_mono(), 0);
    lv_obj_center(label);
    return btn;
}

lv_obj_t *make_tile(lv_obj_t *parent, const char *title, int x, int y, int w, int h)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_size(tile, w, h);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COL_TILE), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    // A whisper of an edge lifts tiles off the dark ground.
    lv_obj_set_style_border_color(tile, lv_color_hex(COL_PANEL_EDGE), 0);
    lv_obj_set_style_border_opa(tile, LV_OPA_40, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_radius(tile, 20, 0);
    lv_obj_set_style_pad_all(tile, 20, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    if (title != nullptr && title[0] != '\0') {
        lv_obj_t *tag = lv_label_create(tile);
        lv_label_set_text(tag, title);
        lv_obj_set_style_text_color(tag, lv_color_hex(COL_DIM), 0);
        lv_obj_set_style_text_font(tag, font_small(), 0);
        lv_obj_set_style_text_letter_space(tag, 2, 0);
        lv_obj_set_pos(tag, 0, 0);
    }
    return tile;
}

uint32_t persona_accent(const char *persona)
{
    if (persona == nullptr) {
        return COL_MIRA;
    }
    if (std::strcmp(persona, "hermes") == 0) {
        return COL_AMBER;
    }
    if (std::strcmp(persona, "archie") == 0) {
        return COL_SUN;
    }
    if (std::strcmp(persona, "mira") == 0) {
        return COL_CYAN;
    }
    return COL_MIRA;
}

const lv_font_t *font_title()
{
    return &lv_font_montserrat_28;
}

const lv_font_t *font_body()
{
    return &lv_font_montserrat_18;
}

const lv_font_t *font_mono()
{
    return &hack_mono_18;
}

const lv_font_t *font_small()
{
    return &lv_font_montserrat_14;
}

}  // namespace hermes_theme
