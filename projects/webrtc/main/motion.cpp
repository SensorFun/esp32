#include "diagnostics.h"
#include "motion.h"
#include "motion_detector.h"
#include "recorder.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr char TAG[] = "motion";

enum class Kind { boolean, number, select };
struct Option { const char *id; double number; const char *label; };  // id set: string value
struct Field {
    const char *name, *label, *group, *unit, *help;
    Kind kind;
    double min, max, step;
    const Option *options;
    size_t option_count;
    double (*get)(const MotionSettings &);
    void (*set)(MotionSettings &, double);
};

// Detection methods. Add new detectors here and dispatch on settings.method.
constexpr Option kMethods[] = {{"optical_flow", 0, "Optical flow"}};
constexpr Option kCells[] = {{nullptr, 5, "5 px"}, {nullptr, 8, "8 px"}, {nullptr, 10, "10 px"}, {nullptr, 20, "20 px"}};

#define FIELD_BOOL(member) [](const MotionSettings &s) { return double(s.member); }, [](MotionSettings &s, double v) { s.member = v != 0; }
#define FIELD_INT(member) [](const MotionSettings &s) { return double(s.member); }, [](MotionSettings &s, double v) { s.member = int(std::lround(v)); }
#define FIELD_FLOAT(member) [](const MotionSettings &s) { return double(s.member); }, [](MotionSettings &s, double v) { s.member = float(v); }
const Field kFields[] = {
    {"enabled", "Detect motion", "detection", "", "Analyse the camera continuously, even with no viewer connected.", Kind::boolean, 0, 1, 1, nullptr, 0, FIELD_BOOL(enabled)},
    {"method", "Method", "detection", "", "Detection algorithm.", Kind::select, 0, 0, 1, kMethods, std::size(kMethods), FIELD_INT(method)},
    // Stored as min_speed: an older min_flow value (pixels per frame) is not reinterpreted.
    {"min_speed", "Minimum speed", "detection", "px/s", "Speed in analysis-image pixels that marks a cell as moving. Independent of the analysis rate.", Kind::number, 0.25, 40, 0.25, nullptr, 0, FIELD_FLOAT(min_speed)},
    {"min_area", "Moving area", "detection", "%", "Share of cells that must move to report motion. 30% of the picture is always enough.", Kind::number, 1, 30, 0.5, nullptr, 0, FIELD_FLOAT(min_area)},
    {"cell", "Cell size", "detection", "px", "Analysis grid in analysis-image pixels. Smaller cells notice smaller objects.", Kind::select, 0, 0, 1, kCells, std::size(kCells), FIELD_INT(cell)},
    {"min_texture", "Minimum texture", "detection", "", "Skips flat cells where flow is unreliable. Higher values ignore more of the scene.", Kind::number, 0, 50, 0.5, nullptr, 0, FIELD_FLOAT(min_texture)},
    {"blur", "Smoothing", "detection", "px", "Blur radius before measuring flow; reduces sensor noise.", Kind::number, 0, 2, 1, nullptr, 0, FIELD_INT(blur)},
    {"trigger_seconds", "Motion duration", "detection", "s", "Motion must last this long before recording starts.", Kind::number, 0.1, 10.9, 0.1, nullptr, 0, FIELD_FLOAT(trigger_seconds)},
    {"overlay", "Show motion in preview", "detection", "", "Highlight moving cells over the live picture.", Kind::boolean, 0, 1, 1, nullptr, 0, FIELD_BOOL(overlay)},
    {"record", "Record to SD card", "recording", "", "Store a clip while motion lasts, at the selected camera resolution.", Kind::boolean, 0, 1, 1, nullptr, 0, FIELD_BOOL(record)},
    {"post_seconds", "Keep recording", "recording", "s", "Continue this long after motion stops.", Kind::number, 1, 300, 1, nullptr, 0, FIELD_INT(post_seconds)},
    {"max_clip_seconds", "Maximum clip length", "recording", "s", "Longer motion continues in a new clip.", Kind::number, 10, 900, 1, nullptr, 0, FIELD_INT(max_clip_seconds)},
    {"overwrite_oldest", "Replace oldest clips when full", "recording", "", "Otherwise recording stops when the card is full.", Kind::boolean, 0, 1, 1, nullptr, 0, FIELD_BOOL(overwrite_oldest)},
};
#undef FIELD_BOOL
#undef FIELD_INT
#undef FIELD_FLOAT

SemaphoreHandle_t lock;
MotionSettings settings;
uint32_t generation = 0;  // Changes whenever settings change; resets the detector.
TaskHandle_t task;
std::atomic<bool> busy{false}, media_paused{false};
uint8_t *job = nullptr;
size_t job_capacity = 0, job_length = 0;
int64_t job_time = 0;  // Capture time of the frame in job.
// Average JPEG size at the current stream resolution, measured by the worker.
std::atomic<uint32_t> frame_dimensions{0};  // dimensions: width << 16 | height
// Results, guarded by lock.
motion::Activity activity;
float area = 0, speed = 0, rate = 0;  // speed: mean of moving cells, px/s
int cols = 0, rows = 0;
std::array<uint8_t, motion::kMaxPixels / 25> grid{};

struct Lock {
    Lock() { xSemaphoreTake(lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(lock); }
};

bool read_value(const Field &field, const cJSON *item, double &value) {
    if (field.kind == Kind::boolean) {
        if (!cJSON_IsBool(item)) return false;
        value = cJSON_IsTrue(item);
        return true;
    }
    if (field.kind == Kind::select) {
        for (size_t i = 0; i < field.option_count; ++i) {
            const Option &option = field.options[i];
            if (option.id ? cJSON_IsString(item) && !strcmp(item->valuestring, option.id)
                          : cJSON_IsNumber(item) && item->valuedouble == option.number) {
                value = option.id ? double(i) : option.number;
                return true;
            }
        }
        return false;
    }
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) return false;
    value = item->valuedouble;
    const double steps = (value - field.min) / field.step;
    return value >= field.min - 1e-9 && value <= field.max + 1e-9 && std::fabs(steps - std::round(steps)) < 1e-6;
}

void write_value(cJSON *object, const Field &field, const MotionSettings &values) {
    double value = field.get(values);
    // Floats hold 0.1 steps inexactly (10.9 reads back as 10.8999996...), which
    // read_value would reject when the saved settings are loaded. Write numbers
    // on the field's step grid.
    if (field.kind == Kind::number) value = field.min + std::round((value - field.min) / field.step) * field.step;
    if (field.kind == Kind::boolean) cJSON_AddBoolToObject(object, field.name, value != 0);
    else if (field.kind == Kind::select && field.options[0].id) cJSON_AddStringToObject(object, field.name, field.options[size_t(value)].id);
    else cJSON_AddNumberToObject(object, field.name, value);
}

cJSON *values_json(const MotionSettings &values) {
    cJSON *object = cJSON_CreateObject();
    for (const Field &field : kFields) write_value(object, field, values);
    return object;
}

// Validates every provided field; absent fields keep their current values.
bool parse(const cJSON *object, MotionSettings &out, const char *&error) {
    if (!cJSON_IsObject(object)) { error = "Invalid motion settings"; return false; }
    for (const Field &field : kFields) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, field.name);
        if (!item) continue;
        double value;
        if (!read_value(field, item, value)) { error = field.label; return false; }
        field.set(out, value);
    }
    return true;
}

esp_err_t store(const MotionSettings &values) {
    cJSON *json = values_json(values);
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("motion", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "settings", text);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    cJSON_free(text);
    return err;
}

void load() {
    nvs_handle_t handle;
    if (nvs_open("motion", NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = 0;
    if (nvs_get_str(handle, "settings", nullptr, &size) == ESP_OK && size > 0 && size <= 2048) {
        std::string text(size, '\0');
        if (nvs_get_str(handle, "settings", text.data(), &size) == ESP_OK) {
            cJSON *json = cJSON_Parse(text.c_str());
            // Restore field by field so one out-of-range value (after a firmware
            // update narrowed its range) does not discard the others.
            for (const Field &field : kFields) {
                double value;
                if (read_value(field, cJSON_GetObjectItemCaseSensitive(json, field.name), value)) field.set(settings, value);
            }
            cJSON_Delete(json);
        }
    }
    nvs_close(handle);
}

cJSON *describe(const char *error = nullptr, bool saved = false) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "motion");
    cJSON *fields = cJSON_AddArrayToObject(reply, "fields");
    for (const Field &field : kFields) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", field.name);
        cJSON_AddStringToObject(item, "label", field.label);
        cJSON_AddStringToObject(item, "group", field.group);
        cJSON_AddStringToObject(item, "type", field.kind == Kind::boolean ? "boolean" : field.kind == Kind::select ? "select" : "number");
        if (*field.unit) cJSON_AddStringToObject(item, "unit", field.unit);
        cJSON_AddStringToObject(item, "help", field.help);
        if (field.kind == Kind::number) {
            cJSON_AddNumberToObject(item, "min", field.min);
            cJSON_AddNumberToObject(item, "max", field.max);
            cJSON_AddNumberToObject(item, "step", field.step);
        }
        if (field.kind == Kind::select) {
            cJSON *options = cJSON_AddArrayToObject(item, "options");
            for (size_t i = 0; i < field.option_count; ++i) {
                cJSON *option = cJSON_CreateObject();
                if (field.options[i].id) cJSON_AddStringToObject(option, "value", field.options[i].id);
                else cJSON_AddNumberToObject(option, "value", field.options[i].number);
                cJSON_AddStringToObject(option, "label", field.options[i].label);
                cJSON_AddItemToArray(options, option);
            }
        }
        cJSON_AddItemToArray(fields, item);
    }
    MotionSettings current, defaults;
    { Lock guard; current = settings; }
    cJSON_AddItemToObject(reply, "values", values_json(current));
    cJSON_AddItemToObject(reply, "defaults", values_json(defaults));
    if (saved) cJSON_AddBoolToObject(reply, "saved", true);
    if (error) cJSON_AddStringToObject(reply, "error", error);
    return reply;
}

void use(const MotionSettings &next) {
    Lock guard;
    settings = next;
    ++generation;
    if (!next.enabled) { activity = {}; area = speed = rate = 0; grid.fill(0); }
}

cJSON *save(const MotionSettings &next) {
    if (store(next) != ESP_OK) return describe("Could not save motion settings");
    use(next);
    diagnostic_eventf("info", "Motion settings saved: detection=%d recording=%d", int(next.enabled), int(next.record));
    ESP_LOGI(TAG, "Settings saved: detection %s, recording %s", next.enabled ? "on" : "off", next.record ? "on" : "off");
    return describe(nullptr, true);
}

// Yield between bounded blocks of work so other priority-1 tasks on CPU0 are
// never kept waiting for a whole frame. The idle task (and the watchdog it
// feeds) runs while the task waits for each frame.
void cooperate() { taskYIELD(); }

// QQVGA decodes directly to luma and never touches the scaler. The luma
// frames stay in internal RAM. On the original ESP32, Wi-Fi and TLS need the
// rest of internal RAM, so the decoder's 10 KiB workspace and the scaler's
// row sums go to PSRAM.
uint8_t flow_frames[2][motion::kMaxPixels];
#if CONFIG_IDF_TARGET_ESP32
EXT_RAM_BSS_ATTR
#endif
motion::JpegWorkspace jpeg_workspace;
#if CONFIG_IDF_TARGET_ESP32
EXT_RAM_BSS_ATTR
#endif
motion::GrayScaler jpeg_scaler;
motion::Detector detector(flow_frames[0], flow_frames[1], jpeg_scaler, jpeg_workspace);
uint32_t seen_generation = UINT32_MAX;
int64_t previous = 0;

void analyze(const uint8_t *jpeg, size_t size, int64_t captured) {
    if (media_paused || recorder_manual_state() != ManualRecording::idle) return;
    MotionSettings s;
    uint32_t current_generation;
    { Lock guard; s = settings; current_generation = generation; }
    if (current_generation != seen_generation) {
        detector.reset();
        seen_generation = current_generation;
        previous = 0;
    }
    if (detector.analyze(jpeg, size, captured, s, cooperate) != motion::FrameResult::analyzed)
        return;
    const int64_t now = esp_timer_get_time();
    Lock guard;
    if (generation != current_generation) return;
    activity.update(detector.flow.motion, now);
    area = detector.flow.area; speed = detector.speed;
    cols = detector.flow.cols; rows = detector.flow.rows; grid = detector.flow.moving;
    if (previous) rate = 0.8f * rate + 0.2f * (1e6f / float(now - previous));
    previous = now;
}

void detect(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        analyze(job, job_length, job_time);
        cooperate();
        busy = false;
    }
}
}  // namespace

void motion_start() {
    lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(lock ? ESP_OK : ESP_ERR_NO_MEM);
    load();
    recorder_start();
    // The stack lives in PSRAM so internal RAM can hold the detection buffers;
    // this task never writes flash, which a PSRAM stack would not survive.
    // Analysis has no rate cap: each finished frame admits the next one. It
    // runs on CPU0 at priority 1: the peer worker, camera task and camera
    // interrupt own CPU1, and Wi-Fi and lwIP on CPU0 preempt it. Measured on the
    // ESP32-CAM streaming 1600 x 1200: sharing CPU1 with the worker cut the
    // stream from 1.25 to 0.2 frames per second, and idle priority left
    // analysis only 0.3 frames per second.
    if (xTaskCreatePinnedToCoreWithCaps(detect, "motion", 6144, nullptr, 1, &task, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
}

MotionSettings motion_settings() { Lock guard; return settings; }

bool motion_frame_due() {
    if (media_paused) return false;
    const int64_t now = esp_timer_get_time();
    MotionSettings s = motion_settings();
    if (!s.enabled) return false;
    return !busy || recorder_frame_due(s, now);
}

void motion_feed(const camera_fb_t *frame) {
    if (media_paused) return;
    if (!frame || frame->format != PIXFORMAT_JPEG) return;
    const uint32_t dimensions = uint32_t(frame->width) << 16 | uint32_t(frame->height);
    frame_dimensions = dimensions;
    MotionSettings s = motion_settings();
    if (!s.enabled) return;
    const int64_t now = esp_timer_get_time();
    // Every frame goes to the motion task, including small motion-idle ones
    // (a few KB to copy), so the worker can grab the next frame meanwhile.
    if (!busy) {
        if (frame->len > job_capacity) {
            heap_caps_free(job);
            job = static_cast<uint8_t *>(heap_caps_malloc(frame->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            job_capacity = job ? frame->len : 0;
        }
        if (job) {
            memcpy(job, frame->buf, frame->len);
            job_length = frame->len;
            job_time = now;
            busy = true;
            xTaskNotifyGive(task);
        }
    }
    // Takes frames only while a confirmed event is recorded (never motion-idle ones).
    recorder_feed(frame, s);
}

motion::Activity motion_activity() {
    Lock guard;
    return activity;
}

bool motion_overlay_active() {
    Lock guard;
    return settings.enabled && settings.overlay;
}

cJSON *motion_status(bool include_overlay) {
    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "type", "motion_status");
    const bool suspended = recorder_manual_state() != ManualRecording::idle;
    {
        Lock guard;
        cJSON_AddBoolToObject(status, "enabled", settings.enabled);
        // Viewers warn over the picture when motion should record but the card cannot.
        cJSON_AddBoolToObject(status, "record", settings.record);
        cJSON_AddBoolToObject(status, "analysis_suspended", suspended);
        cJSON_AddStringToObject(status, "method", kMethods[settings.method].id);
        cJSON_AddBoolToObject(status, "motion", settings.enabled && activity.moving);
        cJSON_AddNumberToObject(status, "area", std::round(double(area) * 10) / 10);
        cJSON_AddNumberToObject(status, "speed", std::round(double(speed) * 10) / 10);
        cJSON_AddNumberToObject(status, "fps", std::round(double(rate) * 10) / 10);
        if (const uint32_t dimensions = suspended ? 0 : frame_dimensions.load()) {
            int width, height;
            motion::analysis_size(int(dimensions >> 16), int(dimensions & 0xffff), width, height);
            cJSON_AddNumberToObject(status, "analysis_width", width);
            cJSON_AddNumberToObject(status, "analysis_height", height);
        }
        if (include_overlay && settings.enabled && settings.overlay && cols && rows) {
            // Row-major moving-cell bits, packed into hex nibbles, MSB first.
            const size_t cells = size_t(cols) * rows;
            std::string hex((cells + 3) / 4, '0');
            for (size_t i = 0; i < cells; ++i) if (grid[i]) hex[i / 4] |= 1 << (3 - i % 4);
            for (char &c : hex) c = "0123456789abcdef"[c - '0'];
            cJSON_AddNumberToObject(status, "cols", cols);
            cJSON_AddNumberToObject(status, "rows", rows);
            cJSON_AddStringToObject(status, "grid", hex.c_str());
        }
    }
    recorder_status(status);
    return status;
}

cJSON *motion_command(const cJSON *request) {
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    if (!cJSON_IsString(op)) return nullptr;
    const bool start_manual = !strcmp(op->valuestring, "recording_start");
    if (start_manual || !strcmp(op->valuestring, "recording_stop")) {
        const char *error = recorder_set_manual(start_manual);
        if (start_manual && !error) {
            // The worker is the only producer. Let any last analysis finish
            // before taking the first manual frame, then start fresh after Stop.
            {
                Lock guard;
                ++generation;
                activity = {};
                area = speed = rate = 0;
                cols = rows = 0;
                grid.fill(0);
            }
            while (busy.load()) vTaskDelay(1);
        }
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "manual_recording");
        if (error) cJSON_AddStringToObject(reply, "error", error);
        recorder_status(reply);
        return reply;
    }
    if (!strcmp(op->valuestring, "motion_get")) return describe();
    if (!strcmp(op->valuestring, "motion_reset")) {
        MotionSettings defaults;
        return save(defaults);
    }
    // motion_set applies live until reboot, like camera "set"; motion_apply also saves.
    const bool live = !strcmp(op->valuestring, "motion_set");
    if (live || !strcmp(op->valuestring, "motion_apply")) {
        MotionSettings next = motion_settings();
        const cJSON *values = cJSON_GetObjectItemCaseSensitive(request, "settings");
        const char *error = nullptr;
        cJSON *reply = nullptr;
        if (!parse(values, next, error)) {
            std::string message = std::string("Invalid value: ") + error;
            reply = describe(message.c_str());
        }
        if (!reply && live) { use(next); reply = describe(); }
        if (!reply) return save(next);
        if (live) cJSON_AddBoolToObject(reply, "live", true);
        return reply;
    }
    if (!strcmp(op->valuestring, "recordings")) {
        const cJSON *offset = cJSON_GetObjectItemCaseSensitive(request, "offset");
        return recorder_list(cJSON_IsNumber(offset) && offset->valuedouble >= 0 && offset->valuedouble < 100000 ? offset->valueint : 0);
    }
    if (!strcmp(op->valuestring, "sd_format")) {
        const char *error = recorder_request_format();
        cJSON *reply = recorder_list(0);
        if (error) cJSON_AddStringToObject(reply, "error", error);
        return reply;
    }
    if (!strcmp(op->valuestring, "recording_delete")) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(request, "name");
        const char *error = cJSON_IsString(name) ? recorder_delete(name->valuestring) : "Missing recording name";
        cJSON *reply = recorder_list(0);
        if (error) cJSON_AddStringToObject(reply, "error", error);
        return reply;
    }
    return nullptr;
}

void motion_pause(bool paused) {
    media_paused = paused;
    if (paused) while (busy.load()) vTaskDelay(1);
    else { Lock guard; ++generation; }
}
