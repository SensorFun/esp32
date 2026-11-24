#include "diagnostics.h"
#include "camera_controls.h"
#include "camera_frames.h"
#include "camera_timing.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string>
#include <cstring>
#include <cmath>
#include <atomic>
struct Control {
    const char *name;
    int (*set)(sensor_t *, int);
    int (*get)(sensor_t *);
};
static CameraTiming timing;
static constexpr const char *timing_names[] = {"high_light", "mid_light", "low_light"};
#include "camera_profiles.h"
static sensor_t *sensor;
static camera_config_t active_config{}, full_config{};
static framesize_t configured_resolution = FRAMESIZE_QVGA;
static bool motion_idle = false;
static const char *sensor_name;
// The detected sensor's controls, allowed values and defaults
// (config/frame_grabber/sensors/<sensor>.yaml).
static const SensorProfile *profile;
static std::string factory_settings;
static bool stream_images = true;
// True only after camera_start succeeds. A failed start can free the driver,
// leaving `sensor` dangling, so nothing may touch the sensor until then.
static bool sensor_ready = false;
static bool capture_idle;
static constexpr char stream_setting[] = "stream_images";
static esp_err_t restore_settings();
static cJSON *settings_values(bool streaming = stream_images);
static bool is_gc() { return sensor->id.PID == GC0308_PID; }
bool camera_stream_images() { return stream_images; }
static void set_stream_images(bool enabled) {
    if (stream_images && !enabled) camera_stop_stream_prefetch();
    stream_images = enabled;
}

// Pausing capture never writes sensor registers, on any sensor. Standby
// writes (OV2640 COM2, OV3660 SYSTEM_CTROL0, GC0308 output gating) can leave
// the SCCB bus hung: on the OV2640 the write timed out and every frame and
// probe after it failed until sensor power was cut. Idle only stops taking
// frames; the driver keeps capturing into its latest-frame buffers. Resuming
// drops a frame captured while idle so the next one is current.
bool camera_capture_idle(bool idle) {
    if (capture_idle == idle) return true;
    if (!idle) {
        camera_frames_pause();
        if (esp_camera_available_frames()) {
            if (auto *stale = esp_camera_fb_get()) esp_camera_fb_return(stale);
        }
        camera_frames_resume();
    }
    capture_idle = idle;
    return true;
}
static framesize_t max_resolution() {
    if (is_gc()) return FRAMESIZE_VGA;
    return sensor->id.PID == OV2640_PID ? FRAMESIZE_UXGA : FRAMESIZE_QXGA;
}
static bool valid_resolution(int size) {
    return size >= 0 && size <= max_resolution() &&
        resolution[size].width >= 160 && resolution[size].height >= 120;
}
static const ControlSpec *spec(const Control &c) {
    for (size_t i = 0; i < profile->count; ++i) if (!strcmp(profile->controls[i].name, c.name)) return &profile->controls[i];
    return nullptr;
}
static bool supported(const Control &c) { return spec(c) != nullptr; }
// An allowed value: within the range, and one of the listed values for lists.
static bool allowed(const Control &c, const cJSON *value) {
    const ControlSpec *s = spec(c);
    if (!s || !cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
        value->valuedouble != std::floor(value->valuedouble) ||
        value->valuedouble < s->low || value->valuedouble > s->high) return false;
    if (!s->option_count) return true;
    for (size_t i = 0; i < s->option_count; ++i) if (s->options[i].value == value->valueint) return true;
    return false;
}
static bool restore_timing(int profile) { return is_gc() || (timing.initialize(sensor) == 0 && timing.apply(profile) == 0); }

// When the sensor last restarted or changed size (esp_timer us).
static std::atomic<int64_t> restarted_at{0};
static void mark_restart() { restarted_at = esp_timer_get_time(); }
int64_t camera_restarted_at() { return restarted_at; }

static void settle() {
    // A sensor just reset, or moved to a different resolution/exposure, needs
    // a frame or two before auto exposure/gain converge; the first capture is
    // otherwise commonly a blown-out white (or black) frame.
    for (int i = 0; i < 2; ++i) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) break;
        esp_camera_fb_return(fb);
    }
    mark_restart();
}
esp_err_t camera_start(bool restore_saved) {
    sensor_ready = false;
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
        // YUV422 goes to the JPEG encoder without colour conversion.
        cfg.pixel_format = PIXFORMAT_YUV422;
        cfg.frame_size = FRAMESIZE_QVGA;
        // Two buffers: the sensor captures the next frame while the encoder
        // task compresses the latest one.
        cfg.fb_count = 2;
        cfg.grab_mode = CAMERA_GRAB_LATEST;
        err = esp_camera_init(&cfg);
    }
    if (err != ESP_OK) return err;
    active_config = full_config = cfg;
    motion_idle = false;
    configured_resolution = FRAMESIZE_QVGA;
    sensor = esp_camera_sensor_get();
    capture_idle = false;
    if (sensor->id.PID == OV2640_PID) {
        sensor_name = "ov2640";
    } else if (sensor->id.PID == OV3660_PID) {
        sensor_name = "ov3660";
    } else if (sensor->id.PID == GC0308_PID) {
        sensor_name = "gc0308";
    } else return ESP_ERR_NOT_SUPPORTED;
    profile = nullptr;
    for (const auto &candidate : profiles) if (!strcmp(candidate.sensor, sensor_name)) profile = &candidate;
    if (!profile) return ESP_ERR_NOT_SUPPORTED;
    timing.set_clock_doubler(profile->clock_2x);
    if (sensor->set_framesize(sensor, FRAMESIZE_QVGA) || !restore_timing(0)) return ESP_FAIL;
    // The camera clock and the sensor's pixel bus run beside the PCB antenna.
    // At full drive strength their edges cut ESP32-CAM Wi-Fi throughput by
    // more than ten times; the weakest drive still captures cleanly.
    gpio_set_drive_capability(gpio_num_t(XCLK_GPIO_NUM), GPIO_DRIVE_CAP_0);
    if (sensor->id.PID == OV2640_PID && sensor->set_reg(sensor, 0x109, 0x03, 0x00)) return ESP_FAIL;  // COM2: 1x output drive
    // The OV3660 driver sets 4x pad drive (0x302C bits 7:6); 1x captures cleanly.
    if (sensor->id.PID == OV3660_PID && sensor->set_reg(sensor, 0x302c, 0xc0, 0x00)) return ESP_FAIL;
    if (profile->timing_profile >= 0 && timing.apply(profile->timing_profile)) return ESP_FAIL;
    for (const auto &c : controls) {
        const ControlSpec *s = spec(c);
        if (s && c.set(sensor, s->value) != 0) {
            ESP_LOGE("camera", "Cannot apply %s", c.name);
            return ESP_FAIL;
        }
    }
    ESP_LOGI("camera", "Detected %s; YAML defaults applied; %s 320x240", sensor_name, is_gc() ? "YUV422 + software JPEG" : "JPEG");
    cJSON *defaults_json = settings_values(true);
    char *defaults_text = cJSON_PrintUnformatted(defaults_json);
    if (!defaults_text) { cJSON_Delete(defaults_json); return ESP_ERR_NO_MEM; }
    factory_settings = defaults_text;
    cJSON_free(defaults_text); cJSON_Delete(defaults_json);
    if (restore_saved) {
        esp_err_t restore_err = restore_settings();
        if (restore_err != ESP_OK) return restore_err;
    }
    if (stream_images) settle();
    else mark_restart();
    sensor_ready = true;
    return ESP_OK;
}
static bool set_resolution(framesize_t size) {
    if (!camera_capture_idle(false)) return false;
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
            camera_frames_pause();
            const esp_err_t reconfigured = esp_camera_reconfigure(&next);
            camera_frames_resume();
            if (reconfigured != ESP_OK) return false;
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
    const bool applied = apply(size);
    // Restore a working sensor configuration if any part of the update fails.
    if (!applied && !apply(previous)) ESP_LOGE("camera", "Could not restore the previous resolution");
    mark_restart();
    return applied;
}
static cJSON *settings_values(bool streaming) {
    cJSON *value = cJSON_CreateObject();
    cJSON_AddNumberToObject(value, "resolution", configured_resolution);
    cJSON_AddNumberToObject(value, stream_setting, streaming);
    if (!is_gc()) cJSON_AddStringToObject(value, "timing_profile", timing_names[timing.selected()]);
    for (const auto &c : controls) if (supported(c)) cJSON_AddNumberToObject(value, c.name, c.get(sensor));
    return value;
}
static bool valid_stream_setting(const cJSON *value) {
    return cJSON_IsNumber(value) && (value->valuedouble == 0 || value->valuedouble == 1);
}
static bool validate_values(const cJSON *values) {
    if (!cJSON_IsObject(values)) return false;
    const cJSON *stream = cJSON_GetObjectItemCaseSensitive(values, stream_setting);
    if (stream && !valid_stream_setting(stream)) return false;
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(values, "resolution");
    if (!cJSON_IsNumber(size) || !std::isfinite(size->valuedouble) || size->valuedouble != std::floor(size->valuedouble) || !valid_resolution(size->valueint)) return false;
    if (!is_gc()) {
        const cJSON *profile = cJSON_GetObjectItemCaseSensitive(values, "timing_profile");
        if (!cJSON_IsString(profile)) return false;
        bool known = false;
        for (const char *name : timing_names) if (!strcmp(name, profile->valuestring)) known = true;
        if (!known) return false;
    }
    for (const auto &c : controls) if (supported(c) && !allowed(c, cJSON_GetObjectItemCaseSensitive(values, c.name))) return false;
    return true;
}
// Writes only what differs from the running sensor, so Save does not
// disturb the stream with redundant register or clock changes. The timing
// baseline stays valid: it is re-read after every driver reset of CLKRC.
static bool apply_values(const cJSON *values, bool force = false) {
    const int size = cJSON_GetObjectItemCaseSensitive(values, "resolution")->valueint;
    if (!motion_idle && (sensor->status.framesize != size || (force && !is_gc())) &&
        !set_resolution(static_cast<framesize_t>(size))) return false;
    for (const auto &c : controls) {
        if (!supported(c)) continue;
        const int value = cJSON_GetObjectItemCaseSensitive(values, c.name)->valueint;
        if ((force || c.get(sensor) != value) && c.set(sensor, value)) return false;
    }
    if (!is_gc()) {
        const char *profile = cJSON_GetObjectItemCaseSensitive(values, "timing_profile")->valuestring;
        for (int i = 0; i < 3; ++i) {
            if (strcmp(profile, timing_names[i])) continue;
            if ((force || timing.selected() != i) && timing.apply(i) != 0) return false;
            break;
        }
    }
    configured_resolution = static_cast<framesize_t>(size);
    if (const cJSON *stream = cJSON_GetObjectItemCaseSensitive(values, stream_setting)) {
        set_stream_images(stream->valueint != 0);
    }
    return true;
}
static esp_err_t store_values(const cJSON *values) {
    // An older viewer may save a settings object without the stream switch.
    cJSON *completed = nullptr;
    if (!cJSON_HasObjectItem(values, stream_setting)) {
        completed = cJSON_Duplicate(values, true);
        if (!completed || !cJSON_AddNumberToObject(completed, stream_setting, stream_images)) {
            cJSON_Delete(completed);
            return ESP_ERR_NO_MEM;
        }
        values = completed;
    }
    char *text = cJSON_PrintUnformatted(values);
    cJSON_Delete(completed);
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
static cJSON *unavailable() {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "error");
    cJSON_AddStringToObject(reply, "error", "The camera sensor is not responding; recovery is in progress.");
    return reply;
}
cJSON *camera_settings() {
    if (!sensor_ready) return unavailable();
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "settings");
    cJSON_AddStringToObject(reply, "sensor", sensor_name);
    cJSON_AddNumberToObject(reply, "resolution", configured_resolution);
    cJSON_AddNumberToObject(reply, "capture_resolution", sensor->status.framesize);
    cJSON_AddNumberToObject(reply, stream_setting, stream_images);
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
        if (timing.clock_doubler() >= 0) cJSON_AddBoolToObject(reply, "clock_2x", timing.clock_doubler() == 1);
        cJSON_AddStringToObject(reply, "timing_profile_status", sensor->id.PID == OV3660_PID
            ? "High light / Mid light / Low light: native / 2x / 3x line period"
            : "High light / Mid light / Low light: native / 2x / 4x line period");
    }
    // Only this sensor's controls: "switch" (0 Off / 1 On), "list" with
    // [value, label] options, or "range" (a number field between min and max).
    cJSON *array = cJSON_AddArrayToObject(reply, "controls");
    for (const auto &c : controls) {
        const ControlSpec *s = spec(c);
        if (!s) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", c.name);
        cJSON_AddStringToObject(item, "kind", s->kind == Kind::toggle ? "switch" : s->kind == Kind::list ? "list" : "range");
        cJSON_AddNumberToObject(item, "min", s->low); cJSON_AddNumberToObject(item, "max", s->high);
        cJSON_AddNumberToObject(item, "value", c.get(sensor));
        cJSON_AddBoolToObject(item, "supported", true);
        if (s->kind == Kind::list) {
            cJSON *options = cJSON_AddArrayToObject(item, "options");
            for (size_t i = 0; i < s->option_count; ++i) {
                cJSON *option = cJSON_CreateArray();
                cJSON_AddItemToArray(option, cJSON_CreateNumber(s->options[i].value));
                cJSON_AddItemToArray(option, cJSON_CreateString(s->options[i].label));
                cJSON_AddItemToArray(options, option);
            }
        }
        cJSON_AddItemToArray(array, item);
    }
    return reply;
}
// Cuts sensor power through the PWDN line (on the AI Thinker board it switches
// the camera's regulators), clearing a sensor that no longer answers on SCCB.
// The driver's own init pulse is only 10 ms. Boards without the line skip it.
static void power_cycle_sensor() {
    if (PWDN_GPIO_NUM < 0) return;
    const auto pin = gpio_num_t(PWDN_GPIO_NUM);
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(300));
    gpio_set_level(pin, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
}

// User controls to restore after recovery, captured while the sensor was last
// known good: after a failed attempt the sensor can no longer be read.
static constexpr size_t control_count = sizeof(controls) / sizeof(controls[0]);
static struct {
    bool valid = false;
    framesize_t size = FRAMESIZE_QVGA;
    int profile = 0, sensor_id = 0;
    int values[control_count]{};
    bool enabled[control_count]{};
} restore_point;

static void snapshot_controls() {
    if (sensor_ready) {
        restore_point.valid = true;
        restore_point.size = configured_resolution;
        restore_point.profile = timing.selected();
        restore_point.sensor_id = sensor->id.PID;
        for (size_t i = 0; i < control_count; ++i) {
            restore_point.enabled[i] = supported(controls[i]);
            if (restore_point.enabled[i]) restore_point.values[i] = controls[i].get(sensor);
        }
    }
}

static esp_err_t restore_controls(framesize_t size) {
    if (!sensor || sensor->id.PID != restore_point.sensor_id) return ESP_ERR_INVALID_STATE;
    if (!set_resolution(size)) return ESP_FAIL;
    for (size_t i = 0; i < control_count; ++i) {
        if (restore_point.enabled[i] && controls[i].set(sensor, restore_point.values[i]) != 0) return ESP_FAIL;
    }
    if (!is_gc() && timing.selected() != restore_point.profile && timing.apply(restore_point.profile)) return ESP_FAIL;
    return ESP_OK;
}

bool camera_is_motion_idle() { return motion_idle; }

// The 160x120 default capture also serves a stream or recording that asks
// for 160x120, so selecting that resolution never restarts the camera.
static bool default_capture(bool idle) { return idle || configured_resolution == FRAMESIZE_QQVGA; }

bool camera_capture_change_needed(bool idle) {
    return sensor_ready && !is_gc() && default_capture(idle) != motion_idle;
}

esp_err_t camera_select_motion_idle(bool idle) {
    if (!sensor_ready) return ESP_ERR_INVALID_STATE;
    // Raw sensors use a different encoder pipeline. This mode targets native JPEG.
    idle = default_capture(idle);
    if (is_gc() || motion_idle == idle) return ESP_OK;
    snapshot_controls();
    camera_config_t next = full_config;
    if (idle) {
        // Two JPEG slots sized for VGA (about 60 KiB each, from width x
        // height / 5); the sensor is moved to QQVGA before frames are used.
        // The headroom covers QQVGA at the user's JPEG quality, which is
        // restored with the other controls (2 makes far larger frames than the
        // default 12). Two slots keep the sensor capturing while one frame is
        // copied or streamed, as at full resolution. They are in PSRAM: on the
        // ESP32, internal RAM must stay free for the TLS handshake of the next
        // viewer, which arrives in this mode.
        next.frame_size = FRAMESIZE_VGA;
        next.fb_count = 2;
        next.fb_location = CAMERA_FB_IN_PSRAM;
        next.grab_mode = CAMERA_GRAB_LATEST;
    }
    sensor_ready = false;
    camera_frames_pause();
    const auto attempt = [&](bool power_cycle) {
        if (power_cycle) {
            esp_camera_deinit();  // Harmless when the failed attempt already freed the driver.
            power_cycle_sensor();
        }
        esp_err_t result = esp_camera_reconfigure(&next);
        if (result != ESP_OK) return result;
        active_config = next;
        capture_idle = false;
        sensor = esp_camera_sensor_get();
        if (!sensor || sensor->id.PID != restore_point.sensor_id) return ESP_ERR_INVALID_STATE;
        gpio_set_drive_capability(gpio_num_t(XCLK_GPIO_NUM), GPIO_DRIVE_CAP_0);
        const int drive = sensor->id.PID == OV2640_PID
            ? sensor->set_reg(sensor, 0x109, 0x03, 0)
            : sensor->set_reg(sensor, 0x302c, 0xc0, 0);
        // Reinitialize the timing helper before reading its selected profile.
        if (drive || !restore_timing(0)) return ESP_FAIL;
        return restore_controls(idle ? FRAMESIZE_QQVGA : configured_resolution);
    };
    // Each switch re-probes the sensor. On the ESP32-CAM about half of the
    // plain restarts fail: an SCCB write times out ("Failed to set frame
    // size") or the probe finds no sensor. One retry with sensor power cut has
    // cleared every case seen, so it is logged here but is not a camera fault;
    // a second failure goes to the caller's recovery and Diagnostics.
    esp_err_t result = attempt(false);
    if (result != ESP_OK) {
        ESP_LOGW("camera", "Capture mode change failed (%s); retrying with sensor power cycle", esp_err_to_name(result));
        result = attempt(true);
    }
    camera_frames_resume();
    if (result != ESP_OK) return result;
    motion_idle = idle;
    sensor_ready = true;
    settle();
    const auto &size = resolution[sensor->status.framesize];
    ESP_LOGI("camera", "Capture mode: %ux%u JPEG, %s", unsigned(size.width), unsigned(size.height),
             idle ? "default capture" : "selected resolution");
    diagnostic_eventf("info", "Capture mode: %ux%u JPEG, %s", unsigned(size.width), unsigned(size.height),
                      idle ? "default capture" : "selected resolution");
    return ESP_OK;
}

esp_err_t camera_recover(bool power_cycle) {
    snapshot_controls();
    sensor_ready = false;
    camera_frames_pause();
    esp_camera_deinit();  // Harmless when a failed start already freed the driver.
    if (power_cycle) power_cycle_sensor();
    // Without a restore point (the camera never started this boot), restore
    // the saved settings as a normal start would.
    esp_err_t err = camera_start(!restore_point.valid);
    camera_frames_resume();
    if (err != ESP_OK || !restore_point.valid) return err;
    err = restore_controls(restore_point.size);
    if (err != ESP_OK) {
        sensor_ready = false;
        return err;
    }
    configured_resolution = restore_point.size;
    settle();
    return ESP_OK;
}
cJSON *camera_command(const cJSON *request) {
    if (!sensor_ready) return unavailable();
    const auto *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "get")) return camera_settings();
    if (cJSON_IsString(op) && (!strcmp(op->valuestring, "apply") || !strcmp(op->valuestring, "reset_defaults"))) {
        const bool reset = !strcmp(op->valuestring, "reset_defaults");
        cJSON *defaults = reset ? cJSON_Parse(factory_settings.c_str()) : nullptr;
        const cJSON *values = reset ? defaults : cJSON_GetObjectItemCaseSensitive(request, "settings");
        const char *error = "Invalid camera settings";
        if (validate_values(values)) {
            cJSON *previous = settings_values();
            // Program the defaults even when the cached controls already match.
            // Keep the initialized capture driver and its buffers in place.
            const bool applied = apply_values(values, reset);
            if (applied) {
                if (reset || store_values(values) == ESP_OK) {
                    cJSON_Delete(previous); cJSON_Delete(defaults);
                    cJSON *reply = camera_settings();
                    diagnostic_event("info", reset ? "Camera defaults applied" : "Camera settings saved");
                    cJSON_AddBoolToObject(reply, reset ? "reset" : "saved", true);
                    return reply;
                }
                error = "Could not save settings; previous settings restored";
            } else error = "Sensor rejected settings; previous settings restored";
            if (!apply_values(previous, reset)) {
                ESP_LOGE("camera", "Could not restore previous camera settings");
                error = "Could not restore previous camera settings";
            }
            cJSON_Delete(previous);
        }
        diagnostic_event("error", error);
        cJSON_Delete(defaults);
        cJSON *reply = cJSON_CreateObject(); cJSON_AddStringToObject(reply, "type", "error"); cJSON_AddStringToObject(reply, "error", error); return reply;
    }
    const auto *name = cJSON_GetObjectItemCaseSensitive(request, "name");
    const auto *value = cJSON_GetObjectItemCaseSensitive(request, "value");
    const char *error = "Unknown camera command";
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "set") && cJSON_IsString(name) &&
        !strcmp(name->valuestring, stream_setting)) {
        if (valid_stream_setting(value)) {
            set_stream_images(value->valueint != 0);
            return camera_settings();
        }
        error = "Stream camera images must be on or off";
    }
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "set") && cJSON_IsString(name) &&
        !strcmp(name->valuestring, "resolution")) {
        if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
            value->valuedouble != std::floor(value->valuedouble) ||
            !valid_resolution(value->valueint)) {
            error = "Unsupported camera resolution";
        } else if (motion_idle || set_resolution(static_cast<framesize_t>(value->valueint))) {
            configured_resolution = static_cast<framesize_t>(value->valueint);
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
            else if (!allowed(c, value)) error = "Value must be one of the advertised values";
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
