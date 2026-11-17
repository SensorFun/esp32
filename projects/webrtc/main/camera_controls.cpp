#include "camera_controls.h"
#include "camera_timing.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "nvs.h"
#include <string>
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
static camera_config_t active_config{};
static const char *sensor_name;
static std::string factory_settings;
static esp_err_t restore_settings();
static cJSON *settings_values();
static bool is_gc() { return sensor->id.PID == GC0308_PID; }
static framesize_t max_resolution() {
    if (is_gc()) return FRAMESIZE_VGA;
    return sensor->id.PID == OV2640_PID ? FRAMESIZE_UXGA : FRAMESIZE_QXGA;
}
static bool valid_resolution(int size) {
    return size >= 0 && size <= max_resolution() &&
        resolution[size].width >= 160 && resolution[size].height >= 120;
}
static bool supported(const Control &c) {
    if (is_gc()) {
        for (const auto &setting : defaults_gc) if (!strcmp(c.name, setting.name)) return true;
        return false;
    }
    // OV2640 driver has no implementation for these two controls.
    return sensor->id.PID != OV2640_PID || (strcmp(c.name, "sharpness") && strcmp(c.name, "denoise"));
}
static int control_low(const Control &c) { return is_gc() && !strcmp(c.name, "sharpness") ? -2 : c.low; }
static int control_high(const Control &c) { return is_gc() && !strcmp(c.name, "sharpness") ? 2 : c.high; }
static bool restore_timing(int profile) { return is_gc() || (timing.initialize(sensor) == 0 && timing.apply(profile) == 0); }

static void settle() {
    // A sensor just reset, or moved to a different resolution/exposure, needs
    // a frame or two before auto exposure/gain converge; the first capture is
    // otherwise commonly a blown-out white (or black) frame.
    for (int i = 0; i < 2; ++i) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) break;
        esp_camera_fb_return(fb);
    }
}
esp_err_t camera_start(bool restore_saved) {
    camera_config_t cfg{};
    cfg.pin_pwdn = PWDN_GPIO_NUM; cfg.pin_reset = RESET_GPIO_NUM;
    cfg.pin_xclk = XCLK_GPIO_NUM; cfg.pin_sccb_sda = SIOD_GPIO_NUM; cfg.pin_sccb_scl = SIOC_GPIO_NUM;
    cfg.pin_d0 = Y2_GPIO_NUM; cfg.pin_d1 = Y3_GPIO_NUM; cfg.pin_d2 = Y4_GPIO_NUM; cfg.pin_d3 = Y5_GPIO_NUM;
    cfg.pin_d4 = Y6_GPIO_NUM; cfg.pin_d5 = Y7_GPIO_NUM; cfg.pin_d6 = Y8_GPIO_NUM; cfg.pin_d7 = Y9_GPIO_NUM;
    cfg.pin_vsync = VSYNC_GPIO_NUM; cfg.pin_href = HREF_GPIO_NUM; cfg.pin_pclk = PCLK_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000;
    cfg.ledc_timer = LEDC_TIMER_0; cfg.ledc_channel = LEDC_CHANNEL_0;
    // Allocate buffers for the largest supported image, then start at QVGA.
    // The driver clamps QXGA to UXGA when it detects an OV2640.
    cfg.pixel_format = PIXFORMAT_JPEG; cfg.frame_size = FRAMESIZE_QXGA;
    cfg.jpeg_quality = 12; cfg.fb_count = 2; cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = CAMERA_GRAB_LATEST;
    esp_err_t err = esp_camera_init(&cfg);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        // esp_camera_init cleans up a failed JPEG probe. Detect non-JPEG
        // sensors again with raw capture; use raw capture dimensions for the DMA buffer.
        cfg.pixel_format = PIXFORMAT_RGB565;
        cfg.frame_size = FRAMESIZE_QVGA;
        cfg.fb_count = 1;
        cfg.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
        err = esp_camera_init(&cfg);
    }
    if (err != ESP_OK) return err;
    active_config = cfg;
    sensor = esp_camera_sensor_get();
    if (sensor->id.PID == OV2640_PID) {
        sensor_name = "ov2640";
    } else if (sensor->id.PID == OV3660_PID) {
        sensor_name = "ov3660";
    } else if (sensor->id.PID == GC0308_PID) {
        sensor_name = "gc0308";
    } else return ESP_ERR_NOT_SUPPORTED;
    if (sensor->set_framesize(sensor, FRAMESIZE_QVGA) || !restore_timing(0)) return ESP_FAIL;
    // The camera clock and the sensor's pixel bus run beside the PCB antenna.
    // At full drive strength their edges cut ESP32-CAM Wi-Fi throughput by
    // more than ten times; the weakest drive still captures cleanly.
    gpio_set_drive_capability(gpio_num_t(XCLK_GPIO_NUM), GPIO_DRIVE_CAP_0);
    if (sensor->id.PID == OV2640_PID && sensor->set_reg(sensor, 0x109, 0x03, 0x00)) return ESP_FAIL;  // COM2: 1x output drive
    const Default *defaults = is_gc() ? defaults_gc : defaults_ov;
    const size_t default_count = is_gc() ? sizeof(defaults_gc) / sizeof(Default) : sizeof(defaults_ov) / sizeof(Default);
    for (size_t i = 0; i < default_count; ++i) {
        const auto &setting = defaults[i];
        if (!strcmp(setting.name, "timing_profile")) {
            if (timing.apply(setting.value)) return ESP_FAIL;
        }
        for (const auto &c : controls) {
            if (!strcmp(c.name, setting.name) && supported(c) && c.set(sensor, setting.value) != 0) {
                ESP_LOGE("camera", "Cannot apply %s", c.name);
                return ESP_FAIL;
            }
        }
    }
    ESP_LOGI("camera", "Detected %s; YAML defaults applied; %s 320x240", sensor_name, is_gc() ? "RGB565 + software JPEG" : "JPEG");
    cJSON *defaults_json = settings_values();
    char *defaults_text = cJSON_PrintUnformatted(defaults_json);
    if (!defaults_text) { cJSON_Delete(defaults_json); return ESP_ERR_NO_MEM; }
    factory_settings = defaults_text;
    cJSON_free(defaults_text); cJSON_Delete(defaults_json);
    if (restore_saved) {
        esp_err_t restore_err = restore_settings();
        if (restore_err != ESP_OK) return restore_err;
    }
    settle();
    return ESP_OK;
}
static bool set_resolution(framesize_t size) {
    const auto previous = static_cast<framesize_t>(sensor->status.framesize);
    const int profile = timing.selected();
    constexpr size_t count = sizeof(controls) / sizeof(controls[0]);
    int values[count]{};
    for (size_t i = 0; i < count; ++i) if (supported(controls[i])) values[i] = controls[i].get(sensor);
    const bool raw_sensor = is_gc();
    auto apply = [&](framesize_t requested) {
        if (raw_sensor) {
            // Raw DMA lengths are fixed at init, unlike variable JPEG frames.
            // Reconfigure the capture engine with the new raw dimensions.
            camera_config_t next = active_config;
            next.frame_size = requested;
            if (esp_camera_reconfigure(&next) != ESP_OK) return false;
            active_config = next;
            sensor = esp_camera_sensor_get();
        } else if (sensor->id.PID == OV2640_PID && requested == FRAMESIZE_320X320) {
            // Driver 2.1.6 incorrectly selects a <=296-line CIF source for
            // 320x320. Use its raw-window API with the SVGA mode (1), cropping
            // the native 800x600 image to its centered 600x600 square instead.
            if (sensor->set_framesize(sensor, FRAMESIZE_SVGA) ||
                sensor->set_res_raw(sensor, 1, 0, 0, 0, 100, 0, 600, 600, 320, 320, true, true)) return false;
            sensor->status.framesize = requested;
        } else if (sensor->set_framesize(sensor, requested)) return false;
        if (sensor->id.PID == OV2640_PID) {
            // As in Espressif's CameraWebServer, change resolution with
            // set_framesize alone, without disturbing the running stream.
            // Driver 2.1.6 rewrites the mode table, window, pixel clock and
            // CLKRC; image controls keep their registers, except COM7's
            // colour-bar bit, which the mode table overwrites.
            if (sensor->status.colorbar && sensor->set_colorbar(sensor, 1)) return false;
            // CLKRC is native again: record that baseline, and change the clock
            // divider only for a non-native light timing.
            if (timing.initialize(sensor)) return false;
            return profile == 0 || timing.apply(profile) == 0;
        }
        // GC0308 is reconfigured from scratch; the OV3660 driver's reset scope
        // is not verified, so both receive every control again.
        for (size_t i = 0; i < count; ++i) {
            if (supported(controls[i]) && controls[i].set(sensor, values[i])) return false;
        }
        return restore_timing(profile);
    };
    if (apply(size)) return true;
    // Restore a working sensor configuration if any part of the update fails.
    ESP_ERROR_CHECK(apply(previous) ? ESP_OK : ESP_FAIL);
    return false;
}
static cJSON *settings_values() {
    cJSON *value = cJSON_CreateObject();
    cJSON_AddNumberToObject(value, "resolution", sensor->status.framesize);
    if (!is_gc()) cJSON_AddStringToObject(value, "timing_profile", timing_names[timing.selected()]);
    for (const auto &c : controls) if (supported(c)) cJSON_AddNumberToObject(value, c.name, c.get(sensor));
    return value;
}
static bool validate_values(const cJSON *values) {
    if (!cJSON_IsObject(values)) return false;
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(values, "resolution");
    if (!cJSON_IsNumber(size) || !std::isfinite(size->valuedouble) || size->valuedouble != std::floor(size->valuedouble) || !valid_resolution(size->valueint)) return false;
    if (!is_gc()) {
        const cJSON *profile = cJSON_GetObjectItemCaseSensitive(values, "timing_profile");
        if (!cJSON_IsString(profile)) return false;
        bool known = false;
        for (const char *name : timing_names) if (!strcmp(name, profile->valuestring)) known = true;
        if (!known) return false;
    }
    for (const auto &c : controls) if (supported(c)) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(values, c.name);
        if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) || v->valuedouble != std::floor(v->valuedouble) || v->valuedouble < control_low(c) || v->valuedouble > control_high(c)) return false;
    }
    return true;
}
// Writes only what differs from the running sensor, so Save and Reset do not
// disturb the stream with redundant register or clock changes. The timing
// baseline stays valid: it is re-read after every driver reset of CLKRC.
static bool apply_values(const cJSON *values) {
    const int size = cJSON_GetObjectItemCaseSensitive(values, "resolution")->valueint;
    if (sensor->status.framesize != size && !set_resolution(static_cast<framesize_t>(size))) return false;
    for (const auto &c : controls) {
        if (!supported(c)) continue;
        const int value = cJSON_GetObjectItemCaseSensitive(values, c.name)->valueint;
        if (c.get(sensor) != value && c.set(sensor, value)) return false;
    }
    if (is_gc()) return true;
    const char *profile = cJSON_GetObjectItemCaseSensitive(values, "timing_profile")->valuestring;
    for (int i = 0; i < 3; ++i) {
        if (strcmp(profile, timing_names[i])) continue;
        return timing.selected() == i || timing.apply(i) == 0;
    }
    return false;
}
static esp_err_t store_values(const cJSON *values) {
    char *text = cJSON_PrintUnformatted(values);
    if (!text) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("camera_settings", NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, sensor_name, text);
        if (ret == ESP_OK) ret = nvs_commit(handle);
        nvs_close(handle);
    }
    cJSON_free(text);
    return ret;
}
static esp_err_t restore_settings() {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open("camera_settings", NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (ret != ESP_OK) return ret;
    size_t size = 0;
    ret = nvs_get_str(handle, sensor_name, nullptr, &size);
    bool fall_back_to_defaults = false;
    if (ret == ESP_OK && size > 0 && size <= 4096) {
        std::string text(size, '\0');
        ret = nvs_get_str(handle, sensor_name, text.data(), &size);
        cJSON *value = ret == ESP_OK ? cJSON_Parse(text.c_str()) : nullptr;
        if (!validate_values(value) || !apply_values(value)) {
            cJSON *defaults = cJSON_Parse(factory_settings.c_str());
            ret = defaults && apply_values(defaults) ? ESP_OK : ESP_FAIL;
            fall_back_to_defaults = ret == ESP_OK;
            cJSON_Delete(defaults);
            ESP_LOGW("camera", "Saved settings invalid; using configured defaults");
        } else ESP_LOGI("camera", "Restored saved camera settings");
        cJSON_Delete(value);
    } else if (ret == ESP_ERR_NVS_NOT_FOUND || (ret == ESP_OK && size > 4096)) ret = ESP_OK;
    nvs_close(handle);
    // Persist the healed state so an invalid blob (narrowed control range after
    // a firmware update, flash wear) is not silently re-applied and re-rejected
    // on every future boot.
    if (fall_back_to_defaults) {
        cJSON *defaults = cJSON_Parse(factory_settings.c_str());
        store_values(defaults);
        cJSON_Delete(defaults);
    }
    return ret;
}
cJSON *camera_settings() {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "settings");
    cJSON_AddStringToObject(reply, "sensor", sensor_name);
    cJSON_AddNumberToObject(reply, "resolution", sensor->status.framesize);
    cJSON *sizes = cJSON_AddArrayToObject(reply, "resolutions");
    for (int i = FRAMESIZE_96X96; i <= max_resolution(); ++i) {
        if (!valid_resolution(i)) continue;
        cJSON *size = cJSON_CreateObject();
        cJSON_AddNumberToObject(size, "value", i);
        cJSON_AddNumberToObject(size, "width", resolution[i].width);
        cJSON_AddNumberToObject(size, "height", resolution[i].height);
        cJSON_AddItemToArray(sizes, size);
    }
    cJSON *profiles = cJSON_AddArrayToObject(reply, "timing_profiles");
    if (!is_gc()) {
        cJSON_AddStringToObject(reply, "timing_profile", timing_names[timing.selected()]);
        for (const char *name : timing_names) cJSON_AddItemToArray(profiles, cJSON_CreateString(name));
        cJSON_AddNumberToObject(reply, "timing_register", timing.readback());
        cJSON_AddStringToObject(reply, "timing_profile_status", sensor->id.PID == OV3660_PID
            ? "High / Mid / Low light: native / 2x / 3x line period"
            : "High / Mid / Low light: native / 2x / 4x line period");
    }
    cJSON *array = cJSON_AddArrayToObject(reply, "controls");
    for (const auto &c : controls) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", c.name);
        cJSON_AddNumberToObject(item, "min", control_low(c)); cJSON_AddNumberToObject(item, "max", control_high(c));
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
    const auto size = static_cast<framesize_t>(sensor->status.framesize);
    const int expected_sensor = sensor->id.PID;
    for (size_t i = 0; i < count; ++i) {
        enabled[i] = supported(controls[i]);
        if (enabled[i]) values[i] = controls[i].get(sensor);
    }
    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK) return err;
    err = camera_start(false);
    if (err != ESP_OK) return err;
    if (sensor->id.PID != expected_sensor) return ESP_ERR_INVALID_STATE;
    if (!set_resolution(size)) return ESP_FAIL;
    for (size_t i = 0; i < count; ++i) {
        if (enabled[i] && controls[i].set(sensor, values[i]) != 0) return ESP_FAIL;
    }
    // set_resolution re-read the native baseline after its driver reset;
    // re-reading it now would capture the default profile's scaled value.
    if (!is_gc() && timing.selected() != profile && timing.apply(profile)) return ESP_FAIL;
    settle();
    return ESP_OK;
}
cJSON *camera_command(const cJSON *request) {
    const auto *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "get")) return camera_settings();
    if (cJSON_IsString(op) && (!strcmp(op->valuestring, "apply") || !strcmp(op->valuestring, "reset_defaults"))) {
        cJSON *defaults = !strcmp(op->valuestring, "reset_defaults") ? cJSON_Parse(factory_settings.c_str()) : nullptr;
        const cJSON *values = defaults ? defaults : cJSON_GetObjectItemCaseSensitive(request, "settings");
        const char *error = "Invalid camera settings";
        if (validate_values(values)) {
            cJSON *previous = settings_values();
            if (apply_values(values)) {
                if (store_values(values) == ESP_OK) {
                    cJSON_Delete(previous); cJSON_Delete(defaults);
                    cJSON *reply = camera_settings(); cJSON_AddBoolToObject(reply, "saved", true); return reply;
                }
                error = "Could not save settings; previous settings restored";
            } else error = "Sensor rejected settings; previous settings restored";
            ESP_ERROR_CHECK(apply_values(previous) ? ESP_OK : ESP_FAIL);
            cJSON_Delete(previous);
        }
        cJSON_Delete(defaults);
        cJSON *reply = cJSON_CreateObject(); cJSON_AddStringToObject(reply, "type", "error"); cJSON_AddStringToObject(reply, "error", error); return reply;
    }
    const auto *name = cJSON_GetObjectItemCaseSensitive(request, "name");
    const auto *value = cJSON_GetObjectItemCaseSensitive(request, "value");
    const char *error = "Unknown camera command";
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "set") && cJSON_IsString(name) &&
        !strcmp(name->valuestring, "resolution")) {
        if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
            value->valuedouble != std::floor(value->valuedouble) ||
            !valid_resolution(value->valueint)) {
            error = "Unsupported camera resolution";
        } else if (set_resolution(static_cast<framesize_t>(value->valueint))) {
            return camera_settings();
        } else error = "Sensor rejected resolution";
    }
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "set") && cJSON_IsString(name) &&
        !strcmp(name->valuestring, "timing_profile")) {
        error = "Unknown timing profile";
        if (!is_gc() && cJSON_IsString(value)) for (int i = 0; i < 3; ++i) {
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
            else if (!std::isfinite(value->valuedouble) || value->valuedouble < control_low(c) || value->valuedouble > control_high(c) || std::floor(value->valuedouble) != value->valuedouble) error = "Value must be an integer within the advertised range";
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
