#pragma once
#include "esp_err.h"
#include "cJSON.h"
esp_err_t camera_start();
cJSON *camera_settings();
cJSON *camera_command(const cJSON *request);
