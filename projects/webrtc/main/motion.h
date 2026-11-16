#pragma once
#include "cJSON.h"
#include "esp_camera.h"
#include <cstdint>

// Motion detection and SD recording settings. The firmware's field table is
// the single source of truth: viewers render whatever motion_command returns.
struct MotionSettings {
    bool enabled = false;
    int method = 0;              // Index into the detection method list.
    int analysis_fps = 5;
    float min_speed = 2.5f;      // Pixels per second at 160x120, independent of analysis_fps.
    float min_area = 2.0f;       // Percent of cells.
    int cell = 8;
    float min_texture = 4.0f;
    int blur = 1;
    float trigger_seconds = 1.0f;
    bool overlay = true;
    bool record = true;
    int record_fps = 10;
    int pre_seconds = 2;
    int post_seconds = 10;
    int max_clip_seconds = 120;
    bool overwrite_oldest = true;
};

// Loads saved settings and starts the detection and recorder tasks.
void motion_start();
// Whether the worker should capture a frame now even without a viewer.
bool motion_frame_due();
// Called by the worker with every captured JPEG frame; copies what it needs.
void motion_feed(const camera_fb_t *frame);
// Handles motion_* and recording list/delete commands; nullptr for other ops.
cJSON *motion_command(const cJSON *request);
cJSON *motion_status();
// True when viewers draw the moving-cell overlay, so status is sent more often.
bool motion_overlay_active();
MotionSettings motion_settings();
// Motion continuously present since `since`; last seen at `last` (esp_timer us).
void motion_activity(bool &moving, int64_t &since, int64_t &last);
