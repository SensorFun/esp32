#pragma once
#include "esp_err.h"
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
