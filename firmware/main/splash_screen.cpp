#include "splash_screen.hpp"

#include "app_config.hpp"
#include "hermes_theme.hpp"

using namespace hermes_theme;

namespace {

constexpr int kCoreY = 286;
constexpr int kProgressX = 154;
constexpr int kProgressY = 524;
constexpr int kProgressW = LCD_W - (kProgressX * 2);

void opacity_cb(void *var, int32_t value)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(var), static_cast<lv_opa_t>(value), 0);
}

void reveal_cb(void *var, int32_t value)
{
    if (value != 0) {
        lv_obj_remove_flag(static_cast<lv_obj_t *>(var), LV_OBJ_FLAG_HIDDEN);
    }
}

// A delayed visibility flip invalidates an object once. Fading a container by
// style opacity makes LVGL allocate and blend an off-screen layer every frame.
void reveal_after(lv_obj_t *object, uint32_t delay_ms)
{
    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, reveal_cb);
    lv_anim_set_values(&animation, 0, 1);
    lv_anim_set_duration(&animation, 1);
    lv_anim_set_delay(&animation, delay_ms);
    lv_anim_start(&animation);
}

void move_x_cb(void *var, int32_t value)
{
    lv_obj_set_x(static_cast<lv_obj_t *>(var), value);
}

void width_cb(void *var, int32_t value)
{
    lv_obj_set_width(static_cast<lv_obj_t *>(var), value);
}

void arc_rotation_cb(void *var, int32_t value)
{
    lv_arc_set_rotation(static_cast<lv_obj_t *>(var), value);
}

void pulse_size_cb(void *var, int32_t value)
{
    lv_obj_t *object = static_cast<lv_obj_t *>(var);
    lv_obj_set_size(object, value, value);
    lv_obj_align(object, LV_ALIGN_TOP_MID, 0, kCoreY - (value / 2));
}

uint32_t star_hash(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    return value ^ (value >> 16);
}

void animate_rotation(lv_obj_t *arc, int from, int to, uint32_t duration_ms,
                      uint32_t delay_ms)
{
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, arc);
    lv_anim_set_exec_cb(&animation, arc_rotation_cb);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_duration(&animation, duration_ms);
    lv_anim_set_delay(&animation, delay_ms);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);
}

void add_full_screen_space(lv_obj_t *parent)
{
    // Each star is a tiny native LVGL object. Only its own bounds are redrawn;
    // the boot never rotates or uploads a full-screen animated bitmap.
    for (uint32_t i = 0; i < 56; ++i) {
        const uint32_t seed = star_hash(i * 0x9e3779b9U + 17U);
        lv_obj_t *star = lv_obj_create(parent);
        const int size = (seed & 31U) == 0 ? 3 : (seed & 3U) == 0 ? 2 : 1;
        lv_obj_set_size(star, size, size);
        lv_obj_set_pos(star, static_cast<int>(seed % LCD_W),
                       static_cast<int>((seed >> 12) % (LCD_H - 48)));
        lv_obj_set_style_bg_color(star,
                                  lv_color_hex((seed & 7U) == 0 ? 0xFFD4BD : 0x8C8478), 0);
        lv_obj_set_style_bg_opa(star, (seed & 7U) == 0 ? LV_OPA_70 : LV_OPA_40, 0);
        lv_obj_set_style_border_width(star, 0, 0);
        lv_obj_set_style_radius(star, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(star, 0, 0);
        lv_obj_clear_flag(star, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(star, LV_OBJ_FLAG_CLICKABLE);

        if ((seed & 31U) == 0) {
            lv_anim_t twinkle;
            lv_anim_init(&twinkle);
            lv_anim_set_var(&twinkle, star);
            lv_anim_set_exec_cb(&twinkle, opacity_cb);
            lv_anim_set_values(&twinkle, LV_OPA_20, LV_OPA_80);
            lv_anim_set_duration(&twinkle, 900 + (seed % 1000));
            lv_anim_set_delay(&twinkle, seed % 800);
            lv_anim_set_reverse_duration(&twinkle, 700 + (seed % 600));
            lv_anim_set_repeat_count(&twinkle, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_path_cb(&twinkle, lv_anim_path_ease_in_out);
            lv_anim_start(&twinkle);
        }
    }

    const int streak_y[3] = {142, 448, 606};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *streak = lv_obj_create(parent);
        lv_obj_set_size(streak, i == 0 ? 104 : 64, i == 2 ? 1 : 2);
        lv_obj_set_pos(streak, -130, streak_y[i]);
        lv_obj_set_style_bg_color(streak, lv_color_hex(i == 0 ? 0xFFD4BD : 0x8C8478), 0);
        lv_obj_set_style_bg_opa(streak, i == 0 ? LV_OPA_60 : LV_OPA_40, 0);
        lv_obj_set_style_border_width(streak, 0, 0);
        lv_obj_set_style_radius(streak, 1, 0);
        lv_obj_clear_flag(streak, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(streak, LV_OBJ_FLAG_CLICKABLE);

        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, streak);
        lv_anim_set_exec_cb(&animation, move_x_cb);
        lv_anim_set_values(&animation, -130, LCD_W + 130);
        lv_anim_set_duration(&animation, 1900 + (i * 550));
        lv_anim_set_delay(&animation, 500 + (i * 850));
        lv_anim_set_repeat_delay(&animation, 5200 + (i * 1300));
        lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
        lv_anim_start(&animation);
    }
}

lv_obj_t *make_center_label(lv_obj_t *parent, const char *text, uint32_t color,
                            const lv_font_t *font, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

lv_obj_t *make_orbit(lv_obj_t *parent, int diameter, int width, uint32_t color,
                     int value, int rotation)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, diameter, diameter);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, kCoreY - (diameter / 2));
    lv_arc_set_range(arc, 0, 1000);
    lv_arc_set_value(arc, value);
    lv_arc_set_rotation(arc, rotation);
    lv_obj_set_style_arc_width(arc, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COL_PANEL_EDGE), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    return arc;
}

lv_obj_t *make_phase(lv_obj_t *parent, const char *index, const char *title,
                     int x, uint32_t delay_ms)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_set_pos(chip, x, 577);
    lv_obj_set_size(chip, 214, 54);
    lv_obj_set_style_bg_color(chip, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_70, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(COL_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_radius(chip, 3, 0);
    lv_obj_set_style_pad_all(chip, 0, 0);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *number = lv_label_create(chip);
    lv_label_set_text(number, index);
    lv_obj_set_style_text_color(number, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_text_font(number, font_mono(), 0);
    lv_obj_align(number, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *name = lv_label_create(chip);
    lv_label_set_text(name, title);
    lv_obj_set_style_text_color(name, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(name, font_small(), 0);
    lv_obj_set_style_text_letter_space(name, 2, 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 52, 0);
    reveal_after(chip, delay_ms);
    return chip;
}

void add_nexus_loader(lv_obj_t *parent)
{
    // Three independently rotating partial rings read as a dimensional portal
    // while touching only a 306px square. This follows LVGL's intended
    // animation model: animate cheap properties on small objects.
    lv_obj_t *outer = make_orbit(parent, 306, 3, COL_SUN, 226, 14);
    lv_obj_t *middle = make_orbit(parent, 238, 4, COL_AMBER, 355, 208);
    lv_obj_t *inner = make_orbit(parent, 168, 3, COL_CYAN, 184, 322);
    animate_rotation(outer, 14, 374, 4600, 120);
    animate_rotation(middle, 568, 208, 3300, 240);
    animate_rotation(inner, 322, 682, 2300, 420);

    lv_obj_t *halo = lv_obj_create(parent);
    lv_obj_set_size(halo, 106, 106);
    lv_obj_align(halo, LV_ALIGN_TOP_MID, 0, kCoreY - 53);
    lv_obj_set_style_bg_color(halo, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_90, 0);
    lv_obj_set_style_border_color(halo, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_border_width(halo, 1, 0);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, halo);
    lv_anim_set_exec_cb(&pulse, pulse_size_cb);
    lv_anim_set_values(&pulse, 100, 116);
    lv_anim_set_duration(&pulse, 850);
    lv_anim_set_delay(&pulse, 420);
    lv_anim_set_reverse_duration(&pulse, 850);
    lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&pulse, lv_anim_path_ease_in_out);
    lv_anim_start(&pulse);

    lv_obj_t *nvs = make_center_label(parent, "NVS", COL_TEXT, &lv_font_montserrat_28,
                                      kCoreY - 22);
    lv_obj_set_style_text_letter_space(nvs, 7, 0);
    lv_obj_t *lock = make_center_label(parent, "NEXUS LOCK", COL_DIM, font_small(),
                                       kCoreY + 19);
    lv_obj_set_style_text_letter_space(lock, 3, 0);

    // The full-width rail communicates actual boot time without redrawing the
    // scene. The status text beneath remains live and is updated by app_main.
    lv_obj_t *track = lv_obj_create(parent);
    lv_obj_set_pos(track, kProgressX, kProgressY);
    lv_obj_set_size(track, kProgressW, 7);
    lv_obj_set_style_bg_color(track, lv_color_hex(COL_PANEL_EDGE), 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_50, 0);
    lv_obj_set_style_border_width(track, 0, 0);
    lv_obj_set_style_radius(track, 3, 0);
    lv_obj_set_style_pad_all(track, 0, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *fill = lv_obj_create(track);
    lv_obj_set_pos(fill, 0, 0);
    lv_obj_set_size(fill, 1, 7);
    lv_obj_set_style_bg_color(fill, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(fill, 0, 0);
    lv_obj_set_style_radius(fill, 3, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_t progress;
    lv_anim_init(&progress);
    lv_anim_set_var(&progress, fill);
    lv_anim_set_exec_cb(&progress, width_cb);
    lv_anim_set_values(&progress, 1, kProgressW);
    lv_anim_set_duration(&progress, 2400);
    lv_anim_set_delay(&progress, 250);
    lv_anim_set_path_cb(&progress, lv_anim_path_ease_out);
    lv_anim_start(&progress);

    make_phase(parent, "01", "DISPLAY", 154, 350);
    make_phase(parent, "02", "PROFILE", 390, 950);
    make_phase(parent, "03", "RADIO", 626, 1550);
    make_phase(parent, "04", "NEXUS", 862, 2150);
}

}  // namespace

void SplashScreen::show(const char *status)
{
    hermes_theme::init();
    screen_ = lv_obj_create(nullptr);
    lv_obj_set_size(screen_, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_clear_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);

    add_full_screen_space(screen_);

    lv_obj_t *protocol = make_center_label(screen_, "NVS-NEBULA // GATEWAY BOOTSTRAP",
                                           COL_DIM, font_mono(), 18);
    lv_obj_set_style_text_letter_space(protocol, 2, 0);

    lv_obj_t *title = make_center_label(screen_, "ARCHIE", COL_TEXT,
                                        &lv_font_montserrat_24, 50);
    lv_obj_set_style_text_letter_space(title, 8, 0);

    add_nexus_loader(screen_);

    status_label_ = make_center_label(screen_,
                                      status != nullptr ? status : "COSMIC DUST CONDENSING",
                                      COL_DIM, font_small(), 543);
    lv_obj_set_style_text_letter_space(status_label_, 2, 0);

    lv_obj_t *operational = make_center_label(screen_, "NEXUS ANCHORED  //  OPERATIONAL",
                                              COL_YELLOW, font_mono(), 646);
    lv_obj_set_style_text_letter_space(operational, 2, 0);
    reveal_after(operational, 2600);

    lv_obj_t *deck = lv_obj_create(screen_);
    lv_obj_set_size(deck, LCD_W, 46);
    lv_obj_align(deck, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(deck, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_bg_opa(deck, LV_OPA_90, 0);
    lv_obj_set_style_border_color(deck, lv_color_hex(COL_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(deck, 1, 0);
    lv_obj_set_style_radius(deck, 0, 0);
    lv_obj_set_style_pad_all(deck, 0, 0);
    lv_obj_clear_flag(deck, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *identity = lv_label_create(deck);
    lv_label_set_text(identity, "ARCHIE  •  NVS-SERIES  •  GATEWAY GUARDIAN");
    lv_obj_set_style_text_color(identity, lv_color_hex(COL_SUN), 0);
    lv_obj_set_style_text_font(identity, font_mono(), 0);
    lv_obj_align(identity, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_t *author = lv_label_create(deck);
    lv_label_set_text(author, "CREATED BY  @AVERROESKW");
    lv_obj_set_style_text_color(author, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_font(author, font_small(), 0);
    lv_obj_align(author, LV_ALIGN_RIGHT_MID, -28, 0);
    reveal_after(deck, 2650);
    lv_scr_load(screen_);
}

void SplashScreen::set_status(const char *status)
{
    if (status_label_ != nullptr && status != nullptr) {
        lv_label_set_text(status_label_, status);
    }
}

void SplashScreen::dismiss()
{
    if (screen_ != nullptr) {
        lv_obj_delete(screen_);
        screen_ = nullptr;
    }
    status_label_ = nullptr;
}

void SplashScreen::forget()
{
    screen_ = nullptr;
    status_label_ = nullptr;
}
