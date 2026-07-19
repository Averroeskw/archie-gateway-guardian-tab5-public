#pragma once

#include "lvgl.h"

#include <cstdint>

enum class ArchieVisualState : uint8_t {
    Boot,
    Idle,
    Linking,
    Online,
    Thinking,
    Error,
};

// Runtime point-cloud character renderer. The model is a compact surface sample
// from the supplied GLB, not a bitmap, so Archie can condense, breathe, rotate,
// and react to link state using one pre-initialized RGB565 canvas in PSRAM.
struct ArchiePointCloud {
    lv_obj_t *canvas = nullptr;
    uint8_t *buffer = nullptr;
    lv_timer_t *timer = nullptr;
    int width = 0;
    int height = 0;
    uint16_t bg565 = 0;
    uint32_t tick = 0;
    uint16_t pulse = 0;
    ArchieVisualState state = ArchieVisualState::Boot;
    bool entrance = false;
};

ArchiePointCloud *archie_pointcloud_create(lv_obj_t *parent, int width, int height,
                                            uint32_t bg_hex, bool play_entrance);
void archie_pointcloud_set_state(ArchiePointCloud *archie, ArchieVisualState state);
void archie_pointcloud_pulse(ArchiePointCloud *archie);
void archie_pointcloud_replay_entrance(ArchiePointCloud *archie);
void archie_pointcloud_destroy(ArchiePointCloud *archie);
void archie_pointcloud_forget(ArchiePointCloud *archie);
