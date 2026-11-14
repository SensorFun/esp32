#pragma once
#include "esp_err.h"
#include "cJSON.h"
esp_err_t camera_start();
// Call only with no outstanding framebuffer; preserve current user controls.
esp_err_t camera_recover();
cJSON *camera_settings();
cJSON *camera_command(const cJSON *request);
