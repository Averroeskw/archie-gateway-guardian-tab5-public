#include "archie_pointcloud.hpp"

#include "assets/archie_pointcloud_data.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <algorithm>
#include <cmath>

namespace {

const char *TAG = "archie_cloud";

// The entrance gets a fluid 30 FPS cadence, then the settled guardian drops
// to 15 FPS. Its breathing motion is intentionally subtle, and the lower idle
// rate leaves the LVGL task ample time for chat scrolling and touch input.
constexpr uint32_t kEntranceFrameMs = 33;
constexpr uint32_t kIdleFrameMs = 66;
constexpr uint32_t kGatherStart = 8;
constexpr uint32_t kGatherEnd = 82;
constexpr uint32_t kIgnitionEnd = 102;

uint16_t rgb565(uint32_t hex)
{
    return lv_color_to_u16(lv_color_hex(hex));
}

uint16_t dim565(uint16_t color, int shift)
{
    while (shift-- > 0) {
        color = (color >> 1) & 0x7bef;
    }
    return color;
}

void plot(uint16_t *fb, int width, int height, int x, int y, int size, uint16_t color)
{
    for (int dy = 0; dy < size; ++dy) {
        const int py = y + dy;
        if (py < 0 || py >= height) {
            continue;
        }
        uint16_t *row = fb + py * width;
        for (int dx = 0; dx < size; ++dx) {
            const int px = x + dx;
            if (px >= 0 && px < width) {
                row[px] = color;
            }
        }
    }
}

void line(uint16_t *fb, int width, int height, int x0, int y0, int x1, int y1,
          uint16_t color)
{
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(fb, width, height, x0, y0, 1, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = err * 2;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void ellipse(uint16_t *fb, int width, int height, int cx, int cy, int rx, int ry,
             uint16_t color)
{
    constexpr int kSegments = 72;
    int last_x = cx + rx;
    int last_y = cy;
    for (int i = 1; i <= kSegments; ++i) {
        const float angle = static_cast<float>(i) * 2.0f * 3.14159265f / kSegments;
        const int x = cx + static_cast<int>(std::cos(angle) * rx);
        const int y = cy + static_cast<int>(std::sin(angle) * ry);
        line(fb, width, height, last_x, last_y, x, y, color);
        last_x = x;
        last_y = y;
    }
}

uint32_t hash32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    return value ^ (value >> 16);
}

float smootherstep(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
}

void free_fading_buffer_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        heap_caps_free(lv_event_get_user_data(event));
    }
}

uint16_t state_color(ArchieVisualState state)
{
    switch (state) {
    case ArchieVisualState::Online:
        return rgb565(0x55e6b5);
    case ArchieVisualState::Thinking:
        return rgb565(0x9c7cff);
    case ArchieVisualState::Error:
        return rgb565(0xff5a6f);
    case ArchieVisualState::Linking:
        return rgb565(0x66d9ef);
    case ArchieVisualState::Boot:
        return rgb565(0xffb454);
    case ArchieVisualState::Idle:
    default:
        return rgb565(0xd8b8ad);
    }
}

void draw_space(ArchiePointCloud *archie, uint16_t *fb)
{
    const uint16_t star_dim = dim565(rgb565(0xc8d3e0), 2);
    const uint16_t star_hot = dim565(rgb565(0xffd4bd), 1);
    const uint32_t star_count = std::clamp<uint32_t>(
        static_cast<uint32_t>(archie->width * archie->height / 7200), 32, 128);
    for (uint32_t i = 0; i < star_count; ++i) {
        const uint32_t seed = hash32(i * 0x9e3779b9U);
        const int speed = 1 + static_cast<int>(i % 3);
        const int x = static_cast<int>((seed + archie->tick * speed) % archie->width);
        const int y = static_cast<int>(((seed >> 12) + archie->tick / (3 + i % 5)) % archie->height);
        const bool hot = (seed & 7U) == 0;
        plot(fb, archie->width, archie->height, x, y, hot ? 2 : 1,
             hot ? star_hot : star_dim);
    }

    // Rare, restrained telemetry streaks make the entire display feel alive.
    if ((archie->tick % 109U) < 18U) {
        const int travel = static_cast<int>((archie->tick % 109U) *
                                            (archie->width + 120) / 18U) - 120;
        line(fb, archie->width, archie->height, travel, archie->height / 5,
             travel + 72, archie->height / 5 - 28, star_hot);
    }
    if ((archie->tick % 157U) > 133U) {
        const int travel = static_cast<int>((archie->tick % 157U - 133U) *
                                            (archie->width + 90) / 23U) - 90;
        line(fb, archie->width, archie->height, travel, archie->height * 4 / 5,
             travel + 52, archie->height * 4 / 5 + 18, star_dim);
    }
}

void render_frame(ArchiePointCloud *archie)
{
    auto *fb = reinterpret_cast<uint16_t *>(archie->buffer);
    std::fill_n(fb, static_cast<size_t>(archie->width) * archie->height, archie->bg565);
    draw_space(archie, fb);

    const uint32_t phase = archie->entrance ? std::min(archie->tick, kIgnitionEnd) : kIgnitionEnd;
    const float raw_gather = (static_cast<float>(phase) - kGatherStart) /
                             static_cast<float>(kGatherEnd - kGatherStart);
    const float gather = archie->entrance ? smootherstep(raw_gather) : 1.0f;
    const float particle_fade = archie->entrance
                                    ? std::clamp(static_cast<float>(phase) / kGatherStart, 0.0f, 1.0f)
                                    : 1.0f;

    // The GLB sampler preserves Archie's real proportions in a 1024-high
    // model box. A single scale keeps the model undistorted on every canvas.
    const float model_scale = std::min(archie->height * 0.88f / 1024.0f,
                                       archie->width * 0.88f / 820.0f);
    const int cx = archie->width / 2;
    const int cy = archie->height / 2 + static_cast<int>(archie->height * 0.015f);
    const float rotate = std::sin(archie->tick * 0.018f) * 0.035f;
    const float cs = std::cos(rotate), sn = std::sin(rotate);
    float breathe = 1.0f + std::sin(archie->tick * 0.050f) * 0.006f;
    if (archie->state == ArchieVisualState::Thinking) {
        breathe += std::sin(archie->tick * 0.15f) * 0.014f;
    }

    const uint16_t hot = state_color(archie->state);
    const uint16_t warm = rgb565(0xd7b7ad);
    const uint16_t cool = rgb565(0xaed8db);
    const uint16_t trail_color = dim565(hot, 2);

    for (size_t i = 0; i < kArchiePointVertexCount; ++i) {
        const ArchiePointVertex &vertex = kArchiePointVertices[i];
        const float rx = vertex.x * cs + vertex.z * sn;
        const float rz = -vertex.x * sn + vertex.z * cs;
        const int target_x = cx + static_cast<int>(rx * model_scale * breathe);
        const int target_y = cy + static_cast<int>(vertex.y * model_scale * breathe);

        const uint32_t seed = hash32(static_cast<uint32_t>(i) ^ 0xa7c41e2dU);
        const int origin_x = static_cast<int>(seed % static_cast<uint32_t>(archie->width));
        const int origin_y = static_cast<int>((seed >> 11) % static_cast<uint32_t>(archie->height));
        const int x = static_cast<int>(origin_x + (target_x - origin_x) * gather);
        const int y = static_cast<int>(origin_y + (target_y - origin_y) * gather);

        // One in six particles leaves a short tail while streaming inward.
        // This creates motion continuity without thousands of draw lines.
        if (archie->entrance && gather > 0.03f && gather < 0.96f && i % 6U == 0U) {
            const float prior = smootherstep(raw_gather - 0.035f);
            const int px = static_cast<int>(origin_x + (target_x - origin_x) * prior);
            const int py = static_cast<int>(origin_y + (target_y - origin_y) * prior);
            line(fb, archie->width, archie->height, px, py, x, y, trail_color);
        }

        const int shimmer = static_cast<int>((archie->tick + i * 13U) & 31U);
        uint16_t color = (i % 13U == 0U) ? cool : (i % 5U == 0U) ? warm : hot;
        if (vertex.glow < 126 || shimmer < 5 || particle_fade < 0.45f) {
            color = dim565(color, 1);
        }
        if (rz < -80) {
            color = dim565(color, 1);
        }
        const int size = (vertex.glow > 206 || archie->pulse > 64) ? 2 : 1;
        plot(fb, archie->width, archie->height, x, y, size, color);
    }

    // The Nexus Deck wakes only after the mesh has settled. It expands as an
    // ellipse across the scene instead of using the previous hard impact line.
    if (phase >= kGatherEnd) {
        const float ignition = smootherstep(static_cast<float>(phase - kGatherEnd) /
                                            static_cast<float>(kIgnitionEnd - kGatherEnd));
        const int deck_y = cy + static_cast<int>(512.0f * model_scale) + 8;
        const int rx = static_cast<int>((archie->width * 0.12f + archie->width * 0.20f * ignition));
        const int ry = std::max(3, static_cast<int>(10.0f * ignition));
        ellipse(fb, archie->width, archie->height, cx, deck_y, rx, ry, dim565(hot, 1));
    }

    if (archie->entrance && phase >= kIgnitionEnd) {
        archie->entrance = false;
        archie->state = ArchieVisualState::Idle;
        archie->pulse = 128;
    }
    if (archie->canvas != nullptr) {
        lv_obj_invalidate(archie->canvas);
    }
}

void frame_cb(lv_timer_t *timer)
{
    auto *archie = static_cast<ArchiePointCloud *>(lv_timer_get_user_data(timer));
    const bool was_entering = archie->entrance;
    ++archie->tick;
    if (archie->pulse > 0) {
        archie->pulse = static_cast<uint16_t>(archie->pulse * 88 / 100);
        if (archie->pulse < 3) {
            archie->pulse = 0;
        }
    }
    render_frame(archie);
    if (was_entering && !archie->entrance) {
        lv_timer_set_period(timer, kIdleFrameMs);
    }
}

}  // namespace

ArchiePointCloud *archie_pointcloud_create(lv_obj_t *parent, int width, int height,
                                            uint32_t bg_hex, bool play_entrance)
{
    auto *archie = new ArchiePointCloud();
    archie->width = width;
    archie->height = height;
    archie->bg565 = rgb565(bg_hex);
    archie->entrance = play_entrance;
    archie->state = play_entrance ? ArchieVisualState::Boot : ArchieVisualState::Idle;
    const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint16_t);
    archie->buffer = static_cast<uint8_t *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (archie->buffer == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for %dx%d point-cloud canvas", width, height);
        delete archie;
        return nullptr;
    }

    // Render before attaching the buffer to LVGL. Previously the canvas was
    // visible for one timer interval with uninitialized PSRAM, producing the
    // white rectangle seen on physical hardware.
    render_frame(archie);

    archie->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(archie->canvas, archie->buffer, width, height, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_style_bg_opa(archie->canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(archie->canvas, 0, 0);
    lv_obj_set_style_pad_all(archie->canvas, 0, 0);
    lv_obj_clear_flag(archie->canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(archie->canvas, LV_OBJ_FLAG_IGNORE_LAYOUT);
    archie->timer = lv_timer_create(frame_cb,
                                    play_entrance ? kEntranceFrameMs : kIdleFrameMs,
                                    archie);
    return archie;
}

void archie_pointcloud_set_state(ArchiePointCloud *archie, ArchieVisualState state)
{
    if (archie != nullptr) {
        archie->state = state;
    }
}

void archie_pointcloud_pulse(ArchiePointCloud *archie)
{
    if (archie != nullptr) {
        archie->pulse = 128;
    }
}

void archie_pointcloud_replay_entrance(ArchiePointCloud *archie)
{
    if (archie != nullptr) {
        archie->tick = 0;
        archie->pulse = 0;
        archie->entrance = true;
        archie->state = ArchieVisualState::Boot;
        if (archie->timer != nullptr) {
            lv_timer_set_period(archie->timer, kEntranceFrameMs);
        }
        render_frame(archie);
    }
}

void archie_pointcloud_destroy(ArchiePointCloud *archie)
{
    if (archie == nullptr) {
        return;
    }
    if (archie->timer != nullptr) {
        lv_timer_delete(archie->timer);
    }
    if (archie->canvas != nullptr) {
        lv_obj_delete(archie->canvas);
    }
    heap_caps_free(archie->buffer);
    delete archie;
}

void archie_pointcloud_forget(ArchiePointCloud *archie)
{
    if (archie == nullptr) {
        return;
    }
    if (archie->timer != nullptr) {
        lv_timer_delete(archie->timer);
    }
    // The canvas is owned by the fading LVGL screen. Release its PSRAM buffer
    // only when LVGL finishes that transition and deletes the canvas.
    if (archie->canvas != nullptr && archie->buffer != nullptr) {
        lv_obj_add_event_cb(archie->canvas, free_fading_buffer_cb, LV_EVENT_DELETE,
                            archie->buffer);
    }
    delete archie;
}
