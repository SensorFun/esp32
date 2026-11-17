#include "camera_frames.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "img_converters.h"
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// JPEG sensors hand their driver frames straight to the worker. Raw sensors
// (GC0308) are compressed by an encoder task on CPU0 that keeps one JPEG ready
// ahead of the worker, so capture and compression overlap sending on CPU1.
// esp_new_jpeg encodes YUV422 without colour conversion (on the S3 about five
// times faster than frame2jpg); frame2jpg remains the fallback.
namespace {
constexpr char TAG[] = "camera_frames";
constexpr int kQuality = 60;
// A frame left unclaimed this long after compression is replaced by a fresh
// one, so occasional consumers (motion detection without a viewer) never
// analyse an old image.
constexpr int64_t kStaleUs = 300000;
// Compression stops when nobody asked for a frame for this long.
constexpr int64_t kIdleUs = 500000;

struct Slot {
    enum State { kFree, kEncoding, kReady, kTaken } state = kFree;
    camera_fb_t fb{};
    uint8_t *buf = nullptr;
    size_t capacity = 0;
    int64_t ready = 0;  // When compression finished.
};
Slot slots[2];
SemaphoreHandle_t slot_lock;     // Guards slot states.
SemaphoreHandle_t capture_lock;  // Recursive: held while a driver frame is out, or the camera is reconfigured.
TaskHandle_t encoder_task, waiting_task;
std::atomic<int64_t> demand{0};

struct Lock {
    Lock() { xSemaphoreTake(slot_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(slot_lock); }
};

void init_locks() {
    static bool done = false;
    if (done) return;
    slot_lock = xSemaphoreCreateMutex();
    capture_lock = xSemaphoreCreateRecursiveMutex();
    configASSERT(slot_lock && capture_lock);
    done = true;
}

bool raw_sensor() {
    sensor_t *sensor = esp_camera_sensor_get();
    return sensor && sensor->pixformat != PIXFORMAT_JPEG;
}

bool reserve(Slot &slot, size_t size) {
    if (slot.capacity >= size) return true;
    jpeg_free_align(slot.buf);
    slot.buf = static_cast<uint8_t *>(jpeg_calloc_align(size, 16));
    slot.capacity = slot.buf ? size : 0;
    return slot.buf != nullptr;
}

jpeg_enc_handle_t encoder;
int encoder_width, encoder_height;
pixformat_t encoder_format;

bool open_encoder(const camera_fb_t *raw) {
    if (encoder && encoder_width == int(raw->width) && encoder_height == int(raw->height) && encoder_format == raw->format) return true;
    if (encoder) { jpeg_enc_close(encoder); encoder = nullptr; }
    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    config.width = raw->width; config.height = raw->height;
    config.src_type = raw->format == PIXFORMAT_YUV422 ? JPEG_PIXEL_FORMAT_YCbYCr : JPEG_PIXEL_FORMAT_RGB565_BE;
    config.subsampling = JPEG_SUBSAMPLE_420;
    config.quality = kQuality;
    if (jpeg_enc_open(&config, &encoder) != JPEG_ERR_OK) { encoder = nullptr; return false; }
    encoder_width = raw->width; encoder_height = raw->height; encoder_format = raw->format;
    return true;
}

bool encode(const camera_fb_t *raw, Slot &slot) {
    // Width x height bytes holds any JPEG of a 2-byte-per-pixel image at this quality.
    if (open_encoder(raw) && reserve(slot, raw->width * raw->height)) {
        int length = 0;
        if (jpeg_enc_process(encoder, raw->buf, raw->len, slot.buf, slot.capacity, &length) == JPEG_ERR_OK && length > 0) {
            slot.fb.len = length;
            return true;
        }
    }
    static bool reported = false;
    if (!reported) { reported = true; ESP_LOGW(TAG, "esp_new_jpeg failed for %ux%u; using frame2jpg", unsigned(raw->width), unsigned(raw->height)); }
    uint8_t *jpeg = nullptr;
    size_t length = 0;
    if (!frame2jpg(const_cast<camera_fb_t *>(raw), kQuality, &jpeg, &length)) { free(jpeg); return false; }
    const bool ok = reserve(slot, length);
    if (ok) { memcpy(slot.buf, jpeg, length); slot.fb.len = length; }
    free(jpeg);
    return ok;
}

void encode_frames(void *) {
    for (;;) {
        Slot *slot = nullptr;
        if (esp_timer_get_time() - demand.load() < kIdleUs) {
            Lock guard;
            bool ready = false;
            for (auto &s : slots) ready |= s.state == Slot::kReady;
            for (auto &s : slots) if (!ready && !slot && s.state == Slot::kFree) slot = &s;
            if (slot) slot->state = Slot::kEncoding;
        }
        if (!slot) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)); continue; }
        xSemaphoreTakeRecursive(capture_lock, portMAX_DELAY);
        camera_fb_t *raw = esp_camera_fb_get();
        bool ok = false;
        if (raw) {
            slot->fb = *raw;
            ok = encode(raw, *slot);
            slot->fb.buf = slot->buf;
            slot->fb.format = PIXFORMAT_JPEG;
            esp_camera_fb_return(raw);
        }
        xSemaphoreGiveRecursive(capture_lock);
        TaskHandle_t wake = nullptr;
        {
            Lock guard;
            slot->state = ok ? Slot::kReady : Slot::kFree;
            slot->ready = esp_timer_get_time();
            wake = waiting_task;
        }
        if (ok && wake) xTaskNotifyGive(wake);
        if (!ok) vTaskDelay(1);
    }
}

void start_encoder() {
    if (encoder_task) return;
    // CPU0 at a low priority: Wi-Fi and lwIP preempt it, and the worker and
    // camera task keep CPU1.
    if (xTaskCreatePinnedToCore(encode_frames, "jpeg_enc", 8192, nullptr, 4, &encoder_task, 0) != pdPASS)
        ESP_LOGE(TAG, "Cannot start the JPEG encoder task");
}
}  // namespace

camera_fb_t *camera_frame_get() {
    init_locks();
    if (!raw_sensor()) return esp_camera_fb_get();
    start_encoder();
    if (!encoder_task) return nullptr;
    const int64_t started = esp_timer_get_time();
    demand = started;
    xTaskNotifyGive(encoder_task);
    // Wait as long as a driver frame wait would (see camera_poll.c).
    while (esp_timer_get_time() - started < 100000) {
        {
            Lock guard;
            waiting_task = nullptr;
            for (auto &slot : slots) {
                if (slot.state != Slot::kReady) continue;
                if (esp_timer_get_time() - slot.ready > kStaleUs) { slot.state = Slot::kFree; continue; }
                slot.state = Slot::kTaken;
                return &slot.fb;
            }
            waiting_task = xTaskGetCurrentTaskHandle();
        }
        xTaskNotifyGive(encoder_task);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    }
    Lock guard;
    waiting_task = nullptr;
    return nullptr;
}

void camera_frame_return(camera_fb_t *frame) {
    if (!frame) return;
    for (auto &slot : slots) {
        if (frame != &slot.fb) continue;
        { Lock guard; slot.state = Slot::kFree; }
        if (encoder_task) xTaskNotifyGive(encoder_task);
        return;
    }
    esp_camera_fb_return(frame);
}

void camera_frames_pause() {
    init_locks();
    xSemaphoreTakeRecursive(capture_lock, portMAX_DELAY);
    // Frames from the old configuration may have another size; drop them.
    Lock guard;
    for (auto &slot : slots) if (slot.state == Slot::kReady) slot.state = Slot::kFree;
}

void camera_frames_resume() { xSemaphoreGiveRecursive(capture_lock); }
