#include "motion.h"
#include "optical_flow.h"
#include "recorder.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"
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
    {"analysis_fps", "Analysis rate", "detection", "fps", "Frames analysed per second. Each is the camera image at 1/8 size, shown in the motion status.", Kind::number, 1, 15, 1, nullptr, 0, FIELD_INT(analysis_fps)},
    // Stored as min_speed: an older min_flow value (pixels per frame) is not reinterpreted.
    {"min_speed", "Minimum speed", "detection", "px/s", "Speed in analysis-image pixels that marks a cell as moving. Independent of the analysis rate.", Kind::number, 0.25, 40, 0.25, nullptr, 0, FIELD_FLOAT(min_speed)},
    {"min_area", "Moving area", "detection", "%", "Share of cells that must move to report motion.", Kind::number, 0.5, 100, 0.5, nullptr, 0, FIELD_FLOAT(min_area)},
    {"cell", "Cell size", "detection", "px", "Analysis grid in analysis-image pixels. Smaller cells notice smaller objects.", Kind::select, 0, 0, 1, kCells, std::size(kCells), FIELD_INT(cell)},
    {"min_texture", "Minimum texture", "detection", "", "Skips flat cells where flow is unreliable. Higher values ignore more of the scene.", Kind::number, 0, 50, 0.5, nullptr, 0, FIELD_FLOAT(min_texture)},
    {"blur", "Smoothing", "detection", "px", "Blur radius before measuring flow; reduces sensor noise.", Kind::number, 0, 2, 1, nullptr, 0, FIELD_INT(blur)},
    {"trigger_seconds", "Motion duration", "detection", "s", "Motion must last this long before recording starts.", Kind::number, 0, 30, 0.5, nullptr, 0, FIELD_FLOAT(trigger_seconds)},
    {"overlay", "Show motion in preview", "detection", "", "Highlight moving cells over the live picture.", Kind::boolean, 0, 1, 1, nullptr, 0, FIELD_BOOL(overlay)},
    {"record", "Record to SD card", "recording", "", "Store a clip while motion lasts, at the selected camera resolution.", Kind::boolean, 0, 1, 1, nullptr, 0, FIELD_BOOL(record)},
    {"record_fps", "Recording rate", "recording", "fps", "Most frames stored per second. Large resolutions may record fewer.", Kind::number, 1, 25, 1, nullptr, 0, FIELD_INT(record_fps)},
    {"pre_seconds", "Pre-record", "recording", "s", "Also keep the seconds before motion was confirmed.", Kind::number, 0, 10, 1, nullptr, 0, FIELD_INT(pre_seconds)},
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
std::atomic<bool> busy{false};
uint8_t *job = nullptr;
size_t job_capacity = 0, job_length = 0;
int64_t job_time = 0;  // Capture time of the frame in job.
int64_t next_analysis = 0;
// Average JPEG size at the current stream resolution, measured by the worker.
std::atomic<uint32_t> frame_bytes{0}, frame_dimensions{0};  // dimensions: width << 16 | height
// Results, guarded by lock.
bool moving = false;
int64_t moving_since = 0, last_motion = 0;
float area = 0, speed = 0, rate = 0;  // speed: mean of moving cells, px/s
int cols = 0, rows = 0;
std::vector<uint8_t> grid;

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
    const double value = field.get(values);
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

int pre_seconds_limit(const MotionSettings &s, int &width, int &height) {
    width = 320; height = 240;
    if (sensor_t *sensor = esp_camera_sensor_get()) {
        const int size = sensor->status.framesize;
        if (size >= 0 && size < FRAMESIZE_INVALID) { width = resolution[size].width; height = resolution[size].height; }
    }
    uint32_t bytes = frame_dimensions == (uint32_t(width) << 16 | uint32_t(height)) ? frame_bytes.load() : 0;
    // Until a frame is measured, assume about 1 byte per 8 pixels (quality 12).
    if (!bytes) bytes = uint32_t(width) * height / 8;
    bytes += bytes / 4;  // Busier scenes produce larger JPEGs.
    // The trigger window and a 0.5 s margin share the buffer with pre-roll;
    // keep a fifth of it for frames waiting to be written.
    const double seconds = 0.8 * double(recorder_budget()) / (double(bytes) * s.record_fps) - s.trigger_seconds - 0.5;
    return std::clamp(int(std::floor(seconds)), 0, 10);
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
            if (!strcmp(field.name, "pre_seconds")) {
                MotionSettings s = motion_settings();
                cJSON_AddNumberToObject(item, "max", motion_pre_seconds_limit(s));
                cJSON_AddStringToObject(item, "max_reason", "free PSRAM, the camera resolution, and the recording rate");
            } else cJSON_AddNumberToObject(item, "max", field.max);
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
    current.pre_seconds = std::min(current.pre_seconds, motion_pre_seconds_limit(current));
    defaults.pre_seconds = std::min(defaults.pre_seconds, motion_pre_seconds_limit(defaults));
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
    if (!next.enabled) { moving = false; area = speed = rate = 0; grid.clear(); }
}

cJSON *save(const MotionSettings &next) {
    if (store(next) != ESP_OK) return describe("Could not save motion settings");
    use(next);
    ESP_LOGI(TAG, "Settings saved: detection %s, recording %s", next.enabled ? "on" : "off", next.record ? "on" : "off");
    return describe(nullptr, true);
}

// Yield to the idle task inside long analysis, even when a high analysis rate
// or a stream of GUI changes keeps work continuously ready.
void cooperate() { vTaskDelay(1); }

// The detector's two luma frames and decoder state live in internal
// RAM (static .bss). Camera JPEGs for streaming and recording, including the
// copy analysed here, stay in PSRAM. On the original ESP32, Wi-Fi needs the
// rest of internal RAM, so the decoder's row sums and workspace go to PSRAM;
// that costs about 5% of analysis time.
#if CONFIG_IDF_TARGET_ESP32
#define MOTION_SCRATCH EXT_RAM_BSS_ATTR
#else
#define MOTION_SCRATCH
#endif
uint8_t flow_frames[2][motion::kMaxPixels];
MOTION_SCRATCH motion::GrayScaler scaler;
MOTION_SCRATCH alignas(4) uint8_t decoder_workspace[3100];

struct DecodeSource { const uint8_t *data; size_t length, read; };

UINT jpeg_input(JDEC *decoder, BYTE *buffer, UINT bytes) {
    auto *source = static_cast<DecodeSource *>(decoder->device);
    bytes = UINT(std::min<size_t>(bytes, source->length - source->read));
    if (buffer) memcpy(buffer, source->data + source->read, bytes);
    source->read += bytes;
    return bytes;
}

// Receives each decoded RGB888 block; edges past the scaled size are clipped.
int decoded_width, decoded_height;
UINT jpeg_output(JDEC *, void *bitmap, JRECT *rect) {
    const int right = std::min<int>(rect->right, decoded_width - 1), bottom = std::min<int>(rect->bottom, decoded_height - 1);
    if (rect->left > right || rect->top > bottom) return 1;
    return scaler.add(static_cast<const uint8_t *>(bitmap), rect->left, rect->top, right, bottom, rect->right - rect->left + 1) ? 1 : 0;
}

// Decodes a JPEG at 1/8 scale straight into an analysis-size luma frame.
// Reports the JPEG's size and the analysis size.
bool decode_gray(const uint8_t *jpeg, size_t length, uint8_t *gray, int &width, int &height, int &out_width, int &out_height) {
    static DecodeSource source;
    static JDEC decoder;  // Decoder state is hot; keep it in internal RAM, not the PSRAM stack.
    source = {jpeg, length, 0};
    if (jd_prepare(&decoder, jpeg_input, decoder_workspace, sizeof(decoder_workspace), &source) != JDR_OK) return false;
    width = decoder.width; height = decoder.height;
    decoded_width = std::max(1, width >> motion::kJpegScale); decoded_height = std::max(1, height >> motion::kJpegScale);
    motion::analysis_size(width, height, out_width, out_height);
    return scaler.begin(decoded_width, decoded_height, out_width, out_height, gray) &&
           jd_decomp(&decoder, jpeg_output, BYTE(motion::kJpegScale)) == JDR_OK && scaler.finish();
}

void detect(void *) {
    motion::OpticalFlow detector(flow_frames[0], flow_frames[1]);
    motion::FlowResult result;
    uint32_t seen_generation = UINT32_MAX;
    int last_width = 0, last_height = 0;
    int64_t previous = 0, previous_frame = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        struct FinishJob {
            ~FinishJob() { busy = false; cooperate(); }
        } finish_job;
        MotionSettings s;
        uint32_t current_generation;
        { Lock guard; s = settings; current_generation = generation; }
        const int64_t frame_time = job_time;
        int width = 0, height = 0, analysis_width = 0, analysis_height = 0;
        if (!s.enabled || !decode_gray(job, job_length, detector.frame(), width, height, analysis_width, analysis_height)) continue;
        if (current_generation != seen_generation || width != last_width || height != last_height) {
            detector.reset();
            seen_generation = current_generation; last_width = width; last_height = height;
            previous_frame = 0;
        }
        // Flow is measured between consecutive analysed frames; convert the
        // speed threshold using their actual capture interval, which may be
        // longer than 1 / analysis_fps when frames arrive slowly.
        const float interval = previous_frame ? std::clamp(float(frame_time - previous_frame) / 1e6f, 0.02f, 5.0f) : 1.0f / s.analysis_fps;
        previous_frame = frame_time;
        motion::FlowParams params;
        params.cell = s.cell; params.min_flow = s.min_speed * interval; params.min_texture = s.min_texture;
        params.blur = s.blur; params.min_area = s.min_area;
        if (!detector.process(analysis_width, analysis_height, params, result, cooperate)) continue;
        const int64_t now = esp_timer_get_time();
        // Brief gaps between detections still count as one continuous motion.
        const int64_t gap = std::max<int64_t>(1000000, 2500000 / s.analysis_fps);
        Lock guard;
        if (generation != current_generation) continue;
        if (result.motion) {
            if (!moving || now - last_motion > gap) moving_since = now;
            moving = true;
            last_motion = now;
        } else if (now - last_motion > gap) moving = false;
        area = result.area; speed = result.mean_flow / interval;
        cols = result.cols; rows = result.rows; grid = result.moving;
        if (previous) rate = 0.8f * rate + 0.2f * (1e6f / float(now - previous));
        previous = now;
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
    // Either core: streaming keeps CPU1 busy, and at this priority analysis
    // only uses time Wi-Fi leaves idle on CPU0.
    if (xTaskCreatePinnedToCoreWithCaps(detect, "motion", 6144, nullptr, 3, &task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
}

MotionSettings motion_settings() { Lock guard; return settings; }

int motion_pre_seconds_limit(const MotionSettings &s) {
    int width, height;
    return pre_seconds_limit(s, width, height);
}

bool motion_frame_due() {
    const int64_t now = esp_timer_get_time();
    MotionSettings s = motion_settings();
    if (!s.enabled) return false;
    return (!busy && now >= next_analysis) || recorder_frame_due(s, now);
}

void motion_feed(const camera_fb_t *frame) {
    if (!frame || frame->format != PIXFORMAT_JPEG) return;
    const uint32_t dimensions = uint32_t(frame->width) << 16 | uint32_t(frame->height);
    if (frame_dimensions != dimensions) { frame_dimensions = dimensions; frame_bytes = uint32_t(frame->len); }
    else frame_bytes = (frame_bytes * 7 + uint32_t(frame->len)) / 8;
    MotionSettings s = motion_settings();
    if (!s.enabled) return;
    const int64_t now = esp_timer_get_time();
    if (!busy && now >= next_analysis) {
        if (frame->len > job_capacity) {
            heap_caps_free(job);
            job = static_cast<uint8_t *>(heap_caps_malloc(frame->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            job_capacity = job ? frame->len : 0;
        }
        if (job) {
            memcpy(job, frame->buf, frame->len);
            job_length = frame->len;
            job_time = now;
            next_analysis = now + 1000000 / s.analysis_fps;
            busy = true;
            xTaskNotifyGive(task);
        }
    }
    recorder_feed(frame, s);
}

void motion_activity(bool &active, int64_t &since, int64_t &last) {
    Lock guard;
    active = moving; since = moving_since; last = last_motion;
}

bool motion_overlay_active() {
    Lock guard;
    return settings.enabled && settings.overlay;
}

cJSON *motion_status() {
    cJSON *status = cJSON_CreateObject();
    cJSON_AddStringToObject(status, "type", "motion_status");
    {
        Lock guard;
        cJSON_AddBoolToObject(status, "enabled", settings.enabled);
        cJSON_AddStringToObject(status, "method", kMethods[settings.method].id);
        cJSON_AddBoolToObject(status, "motion", settings.enabled && moving);
        cJSON_AddNumberToObject(status, "area", std::round(double(area) * 10) / 10);
        cJSON_AddNumberToObject(status, "speed", std::round(double(speed) * 10) / 10);
        cJSON_AddNumberToObject(status, "fps", std::round(double(rate) * 10) / 10);
        if (const uint32_t dimensions = frame_dimensions) {
            int width, height;
            motion::analysis_size(int(dimensions >> 16), int(dimensions & 0xffff), width, height);
            cJSON_AddNumberToObject(status, "analysis_width", width);
            cJSON_AddNumberToObject(status, "analysis_height", height);
        }
        if (settings.enabled && settings.overlay && cols && grid.size() == size_t(cols) * rows) {
            // Row-major moving-cell bits, packed into hex nibbles, MSB first.
            std::string hex((grid.size() + 3) / 4, '0');
            for (size_t i = 0; i < grid.size(); ++i) if (grid[i]) hex[i / 4] |= 1 << (3 - i % 4);
            for (char &c : hex) c = "0123456789abcdef"[c - '0'];
            cJSON_AddNumberToObject(status, "cols", cols);
            cJSON_AddNumberToObject(status, "rows", rows);
            cJSON_AddStringToObject(status, "grid", hex.c_str());
        }
    }
    const MotionSettings s = motion_settings();
    cJSON *limits = cJSON_AddObjectToObject(status, "limits");
    cJSON_AddNumberToObject(limits, "pre_seconds", motion_pre_seconds_limit(s));
    recorder_status(status);
    return status;
}

cJSON *motion_command(const cJSON *request) {
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    if (!cJSON_IsString(op)) return nullptr;
    if (!strcmp(op->valuestring, "motion_get")) return describe();
    if (!strcmp(op->valuestring, "motion_reset")) {
        MotionSettings defaults;
        defaults.pre_seconds = std::min(defaults.pre_seconds, motion_pre_seconds_limit(defaults));
        return save(defaults);
    }
    // motion_set applies live until reboot, like camera "set"; motion_apply also saves.
    const bool live = !strcmp(op->valuestring, "motion_set");
    if (live || !strcmp(op->valuestring, "motion_apply")) {
        MotionSettings next = motion_settings();
        const char *error = nullptr;
        cJSON *reply = nullptr;
        if (!parse(cJSON_GetObjectItemCaseSensitive(request, "settings"), next, error)) {
            std::string message = std::string("Invalid value: ") + error;
            reply = describe(message.c_str());
        }
        int width, height;
        const int limit = reply ? 0 : pre_seconds_limit(next, width, height);
        if (!reply && next.pre_seconds > limit) {
            char message[160];
            snprintf(message, sizeof(message), "Pre-record can be at most %d s at %d x %d and %d fps; free PSRAM limits the buffer. Lower the recording rate or resolution for more.",
                     limit, width, height, next.record_fps);
            reply = describe(message);
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
