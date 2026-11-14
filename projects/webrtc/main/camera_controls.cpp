#include "camera_controls.h"
#include "camera_timing.h"
#include "esp_camera.h"
#include "esp_log.h"
#include <cstring>
#include <cmath>
struct Control {
    const char *name;
    int low, high;
    int (*set)(sensor_t *, int);
    int (*get)(sensor_t *);
};
static CameraTiming timing;
static constexpr const char *timing_names[] = {"high_light", "balanced", "low_light"};
#include "camera_profiles.h"
static sensor_t *sensor;
static const char *sensor_name;
static bool supported(const Control &c) {
    // OV2640 driver has no implementation for these two controls.
    return sensor->id.PID != OV2640_PID || (strcmp(c.name, "sharpness") && strcmp(c.name, "denoise"));
}
esp_err_t camera_start() {
    camera_config_t cfg{};
    cfg.pin_pwdn = PWDN_GPIO_NUM; cfg.pin_reset = RESET_GPIO_NUM;
    cfg.pin_xclk = XCLK_GPIO_NUM; cfg.pin_sccb_sda = SIOD_GPIO_NUM; cfg.pin_sccb_scl = SIOC_GPIO_NUM;
    cfg.pin_d0 = Y2_GPIO_NUM; cfg.pin_d1 = Y3_GPIO_NUM; cfg.pin_d2 = Y4_GPIO_NUM; cfg.pin_d3 = Y5_GPIO_NUM;
    cfg.pin_d4 = Y6_GPIO_NUM; cfg.pin_d5 = Y7_GPIO_NUM; cfg.pin_d6 = Y8_GPIO_NUM; cfg.pin_d7 = Y9_GPIO_NUM;
    cfg.pin_vsync = VSYNC_GPIO_NUM; cfg.pin_href = HREF_GPIO_NUM; cfg.pin_pclk = PCLK_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000;
    cfg.ledc_timer = LEDC_TIMER_0; cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.pixel_format = PIXFORMAT_JPEG; cfg.frame_size = FRAMESIZE_QVGA;
    cfg.jpeg_quality = 12; cfg.fb_count = 2; cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = CAMERA_GRAB_LATEST;
    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) return err;
    sensor = esp_camera_sensor_get();
    const Default *defaults = nullptr;
    size_t count = 0;
    if (sensor->id.PID == OV2640_PID) {
        sensor_name = "ov2640"; defaults = defaults_ov2640; count = sizeof(defaults_ov2640) / sizeof(Default);
    } else if (sensor->id.PID == OV3660_PID) {
        sensor_name = "ov3660"; defaults = defaults_ov3660; count = sizeof(defaults_ov3660) / sizeof(Default);
    } else return ESP_ERR_NOT_SUPPORTED;
    if (timing.initialize(sensor)) return ESP_FAIL;
    for (size_t i = 0; i < count; ++i) {
        if (!strcmp(defaults[i].name, "timing_profile")) {
            if (timing.apply(defaults[i].value)) return ESP_FAIL;
        }
        for (const auto &c : controls) {
        if (!strcmp(c.name, defaults[i].name) && (!supported(c) || c.set(sensor, defaults[i].value) != 0)) {
            ESP_LOGE("camera", "Cannot apply %s", c.name);
            return ESP_FAIL;
        }
    }
    }
    ESP_LOGI("camera", "Detected %s; YAML defaults applied; JPEG 320x240", sensor_name);
    return ESP_OK;
}
cJSON *camera_settings() {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "settings");
    cJSON_AddStringToObject(reply, "sensor", sensor_name);
    cJSON_AddStringToObject(reply, "timing_profile", timing_names[timing.selected()]);
    cJSON *profiles = cJSON_AddArrayToObject(reply, "timing_profiles");
    for (const char *name : timing_names) cJSON_AddItemToArray(profiles, cJSON_CreateString(name));
    cJSON_AddNumberToObject(reply, "timing_register", timing.readback());
    cJSON_AddStringToObject(reply, "timing_profile_status", sensor->id.PID == OV3660_PID
        ? "High / Mid / Low light: native / 2x / 3x line period"
        : "High / Mid / Low light: native / 2x / 4x line period");
    cJSON *array = cJSON_AddArrayToObject(reply, "controls");
    for (const auto &c : controls) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", c.name);
        cJSON_AddNumberToObject(item, "min", c.low); cJSON_AddNumberToObject(item, "max", c.high);
        cJSON_AddNumberToObject(item, "value", c.get(sensor));
        cJSON_AddBoolToObject(item, "supported", supported(c));
        cJSON_AddItemToArray(array, item);
    }
    return reply;
}
esp_err_t camera_recover() {
    constexpr size_t count = sizeof(controls) / sizeof(controls[0]);
    int values[count]{};
    bool enabled[count]{};
    const int profile = timing.selected();
    const int expected_sensor = sensor->id.PID;
    for (size_t i = 0; i < count; ++i) {
        enabled[i] = supported(controls[i]);
        if (enabled[i]) values[i] = controls[i].get(sensor);
    }
    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK) return err;
    err = camera_start();
    if (err != ESP_OK) return err;
    if (sensor->id.PID != expected_sensor) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < count; ++i) {
        if (enabled[i] && controls[i].set(sensor, values[i]) != 0) return ESP_FAIL;
    }
    return timing.apply(profile) == 0 ? ESP_OK : ESP_FAIL;
}
cJSON *camera_command(const cJSON *request) {
    const auto *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "get")) return camera_settings();
    const auto *name = cJSON_GetObjectItemCaseSensitive(request, "name");
    const auto *value = cJSON_GetObjectItemCaseSensitive(request, "value");
    const char *error = "Unknown camera command";
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "set") && cJSON_IsString(name) &&
        !strcmp(name->valuestring, "timing_profile")) {
        error = "Unknown timing profile";
        if (cJSON_IsString(value)) for (int i = 0; i < 3; ++i) {
            if (strcmp(value->valuestring, timing_names[i])) continue;
            int ret = timing.apply(i);
            if (ret == -2) ESP_ERROR_CHECK(ESP_FAIL);
            if (ret == 0) return camera_settings();
            error = "Sensor rejected timing profile";
            break;
        }
    }
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "set") && cJSON_IsString(name) && cJSON_IsNumber(value)) {
        for (const auto &c : controls) if (!strcmp(name->valuestring, c.name)) {
            if (!supported(c)) error = "Control is unsupported by this sensor";
            else if (!std::isfinite(value->valuedouble) || value->valuedouble < c.low || value->valuedouble > c.high || std::floor(value->valuedouble) != value->valuedouble) error = "Value must be an integer within the advertised range";
            else if (c.set(sensor, value->valueint) != 0) error = "Sensor rejected setting";
            else return camera_settings();
            break;
        }
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "error");
    cJSON_AddStringToObject(reply, "error", error);
    return reply;
}
