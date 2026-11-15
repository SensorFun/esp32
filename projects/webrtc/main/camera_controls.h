#pragma once
#include "esp_err.h"
#include "cJSON.h"
#include "camera_frames.h"
esp_err_t camera_start(bool restore_saved = true);
// Call only with no outstanding framebuffer; preserve current user controls.
esp_err_t camera_recover();
cJSON *camera_settings();
cJSON *camera_command(const cJSON *request);
