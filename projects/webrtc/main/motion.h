#pragma once
#include "cJSON.h"
#include "esp_camera.h"
#include <cstdint>

#include "motion_settings.h"

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
// Longest pre-record the PSRAM buffer can hold at the current resolution and
// these settings' recording rate and motion duration (0-10 s).
int motion_pre_seconds_limit(const MotionSettings &settings);
// Motion continuously present since `since`; last seen at `last` (esp_timer us).
void motion_activity(bool &moving, int64_t &since, int64_t &last);

// Wait for any current analysis to finish before pausing.
void motion_pause(bool paused);
