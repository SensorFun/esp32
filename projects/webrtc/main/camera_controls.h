#pragma once
#include "esp_err.h"
#include <cstdint>
#include "cJSON.h"
#include "camera_frames.h"
esp_err_t camera_start(bool restore_saved = true);
// Restarts the driver, keeping the user's current controls. Call only with no
// outstanding framebuffer. `power_cycle` also cuts sensor power first. Safe to
// call again after a failed attempt or a failed camera_start.
esp_err_t camera_recover(bool power_cycle = false);
bool camera_stream_images();
bool camera_capture_idle(bool idle);
cJSON *camera_settings();
cJSON *camera_command(const cJSON *request);

// idle: nothing needs the selected resolution, so capture the 160x120
// default. Call without an outstanding framebuffer when
// camera_capture_change_needed(idle). Keeps the user's selected size.
esp_err_t camera_select_motion_idle(bool idle);
bool camera_capture_change_needed(bool idle);
bool camera_is_motion_idle();
// When the sensor last restarted or changed capture size (esp_timer us). Auto
// exposure and white balance take about a second to settle after that.
int64_t camera_restarted_at();
