#pragma once
#include "cJSON.h"
#include "esp_camera.h"
#include <cstdint>

#include "motion_settings.h"
#include "motion_detector.h"

// Loads saved settings and starts the detection and recorder tasks.
void motion_start();
// Whether the worker should capture a frame now even without a viewer.
bool motion_frame_due();
// Called by the worker with every captured JPEG frame; copies what it needs.
void motion_feed(const camera_fb_t *frame);
// Handles motion_* and recording list/delete commands; nullptr for other ops.
cJSON *motion_command(const cJSON *request);
cJSON *motion_status(bool include_overlay);
// True when viewers draw the moving-cell overlay, so status is sent more often.
bool motion_overlay_active();
MotionSettings motion_settings();
// Motion continuously present since `since`; last seen at `last` (esp_timer us).
motion::Activity motion_activity();

// Wait for any current analysis to finish before pausing.
void motion_pause(bool paused);
