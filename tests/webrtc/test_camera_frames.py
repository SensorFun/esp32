"""Check JPEG passthrough and the raw-sensor encoder pipeline with fake drivers.

FreeRTOS, the camera driver and both JPEG encoders are replaced by small
host fakes (threads, mutexes and condition variables), so the real
camera_frames.cpp runs under AddressSanitizer.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

FAKES = {
    'esp_camera.h': '''#pragma once
#include <cstddef>
#include <cstdint>
enum pixformat_t { PIXFORMAT_RGB565, PIXFORMAT_YUV422, PIXFORMAT_JPEG };
struct camera_fb_t { uint8_t *buf; size_t len, width, height; pixformat_t format; };
struct sensor_t { pixformat_t pixformat; };
camera_fb_t *esp_camera_fb_get();
void esp_camera_fb_return(camera_fb_t *);
bool esp_camera_available_frames();
sensor_t *esp_camera_sensor_get();
''',
    'img_converters.h': '''#pragma once
#include "esp_camera.h"
bool frame2jpg(camera_fb_t *, uint8_t, uint8_t **, size_t *);
''',
    'esp_jpeg_enc.h': '''#pragma once
#include <cstddef>
#include <cstdint>
enum jpeg_pixel_format_t { JPEG_PIXEL_FORMAT_YCbYCr, JPEG_PIXEL_FORMAT_RGB565_BE };
enum jpeg_subsampling_t { JPEG_SUBSAMPLE_420 };
enum jpeg_error_t { JPEG_ERR_OK = 0, JPEG_ERR_FAIL = -1 };
struct jpeg_enc_config_t { int width, height; jpeg_pixel_format_t src_type; jpeg_subsampling_t subsampling; uint8_t quality; };
#define DEFAULT_JPEG_ENC_CONFIG() {}
typedef void *jpeg_enc_handle_t;
jpeg_error_t jpeg_enc_open(jpeg_enc_config_t *, jpeg_enc_handle_t *);
jpeg_error_t jpeg_enc_process(jpeg_enc_handle_t, const uint8_t *, int, uint8_t *, int, int *);
jpeg_error_t jpeg_enc_close(jpeg_enc_handle_t);
void *jpeg_calloc_align(size_t, int);
void jpeg_free_align(void *);
''',
    'esp_log.h': '''#pragma once
#include <cstdio>
#define ESP_LOGE(tag, ...) (std::printf(__VA_ARGS__), std::printf("\\n"))
#define ESP_LOGW(tag, ...) (std::printf(__VA_ARGS__), std::printf("\\n"))
''',
    'esp_timer.h': '''#pragma once
#include <chrono>
#include <cstdint>
inline int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
''',
    'freertos/FreeRTOS.h': '''#pragma once
#include <cassert>
#include <cstdint>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdMS_TO_TICKS(ms) (TickType_t(ms))
#define portMAX_DELAY 0xffffffffu
#define pdPASS 1
#define pdTRUE 1
#define configASSERT(x) assert(x)
''',
    'freertos/semphr.h': '''#pragma once
#include "freertos/FreeRTOS.h"
#include <mutex>
struct FakeSemaphore { std::recursive_mutex mutex; };
typedef FakeSemaphore *SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new FakeSemaphore; }
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return new FakeSemaphore; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t) { s->mutex.lock(); return 1; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { s->mutex.unlock(); return 1; }
inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t) { s->mutex.lock(); return 1; }
inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { s->mutex.unlock(); return 1; }
''',
    'freertos/task.h': '''#pragma once
#include "freertos/FreeRTOS.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
struct FakeTask { std::mutex mutex; std::condition_variable wake; uint32_t count = 0; };
typedef FakeTask *TaskHandle_t;
inline TaskHandle_t &fake_current() { thread_local TaskHandle_t task = new FakeTask; return task; }
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return fake_current(); }
inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *, uint32_t, void *arg, int, TaskHandle_t *out, int) {
    TaskHandle_t task = new FakeTask;
    *out = task;
    std::thread([fn, arg, task] { fake_current() = task; fn(arg); }).detach();
    return pdPASS;
}
inline void xTaskNotifyGive(TaskHandle_t task) { { std::lock_guard<std::mutex> g(task->mutex); ++task->count; } task->wake.notify_all(); }
inline uint32_t ulTaskNotifyTake(BaseType_t, TickType_t ticks) {
    TaskHandle_t task = fake_current();
    std::unique_lock<std::mutex> lock(task->mutex);
    task->wake.wait_for(lock, std::chrono::milliseconds(ticks), [&] { return task->count > 0; });
    uint32_t value = task->count; task->count = 0; return value;
}
inline void vTaskDelay(TickType_t ticks) { std::this_thread::sleep_for(std::chrono::milliseconds(ticks)); }
''',
}

TEST = r'''#include "camera_frames.h"
#include "esp_jpeg_enc.h"
#include "img_converters.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static uint8_t pixels[2][8]{};
static camera_fb_t frames[2]{{pixels[0], 8, 2, 2, PIXFORMAT_YUV422}, {pixels[1], 8, 2, 2, PIXFORMAT_YUV422}};
static sensor_t sensor{PIXFORMAT_JPEG};
static std::atomic<bool> absent{false}, fast_fails{false}, slow_fails{false}, paused{false};
static std::atomic<int> outstanding{0}, gets{0}, sequence{0}, slow_uses{0};

sensor_t *esp_camera_sensor_get() { return &sensor; }
camera_fb_t *esp_camera_fb_get() {
    assert(!paused);  // The encoder must not touch the driver while paused.
    if (absent) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); return nullptr; }
    camera_fb_t *fb = &frames[gets++ % 2];
    fb->buf[0] = uint8_t(++sequence);  // Lets the test tell fresh frames from old ones.
    fb->format = sensor.pixformat == PIXFORMAT_JPEG ? PIXFORMAT_JPEG : PIXFORMAT_YUV422;
    ++outstanding;
    return fb;
}
void esp_camera_fb_return(camera_fb_t *fb) { assert(fb == &frames[0] || fb == &frames[1]); --outstanding; }
bool esp_camera_available_frames() { return !absent; }

jpeg_error_t jpeg_enc_open(jpeg_enc_config_t *config, jpeg_enc_handle_t *out) {
    assert(config->width == 2 && config->height == 2 && config->quality == 60 && config->src_type == JPEG_PIXEL_FORMAT_YCbYCr);
    *out = reinterpret_cast<jpeg_enc_handle_t>(1); return JPEG_ERR_OK;
}
jpeg_error_t jpeg_enc_process(jpeg_enc_handle_t, const uint8_t *in, int, uint8_t *out, int capacity, int *length) {
    if (fast_fails || capacity < 4) return JPEG_ERR_FAIL;
    out[0] = 0xff; out[1] = 0xd8; out[2] = in[0]; out[3] = 0; *length = 4; return JPEG_ERR_OK;
}
jpeg_error_t jpeg_enc_close(jpeg_enc_handle_t) { return JPEG_ERR_OK; }
void *jpeg_calloc_align(size_t size, int) { return calloc(1, size); }
void jpeg_free_align(void *p) { free(p); }
bool frame2jpg(camera_fb_t *fb, uint8_t quality, uint8_t **data, size_t *length) {
    assert(quality == 60);
    ++slow_uses;
    if (slow_fails) return false;
    *data = static_cast<uint8_t *>(malloc(5));
    (*data)[0] = 0xff; (*data)[1] = 0xd8; (*data)[2] = fb->buf[0]; (*data)[3] = 1; (*data)[4] = 0;
    *length = 5;
    return true;
}
static void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

int main() {
    // JPEG sensors: the driver frame itself, released on return.
    camera_fb_t *fb = camera_frame_get();
    assert(fb == &frames[0] && outstanding == 1);
    camera_frame_return(fb); assert(outstanding == 0);

    // Raw sensors: compressed copies; driver frames go back as soon as encoded.
    sensor.pixformat = PIXFORMAT_YUV422;
    int last = 0;
    for (int i = 0; i < 40; ++i) {
        fb = camera_frame_get();
        assert(fb && fb != &frames[0] && fb != &frames[1] && fb->format == PIXFORMAT_JPEG);
        assert(fb->width == 2 && fb->height == 2 && fb->len == 4 && fb->buf[0] == 0xff && fb->buf[1] == 0xd8);
        assert(fb->buf[2] != last);  // Never the same frame twice.
        last = fb->buf[2];
        camera_frame_return(fb);
    }
    settle(); assert(outstanding == 0);

    // A frame left unclaimed goes stale: the next one is freshly captured.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const int before = sequence;
    fb = camera_frame_get(); assert(fb && fb->buf[2] > uint8_t(before)); camera_frame_return(fb);

    // esp_new_jpeg failing falls back to frame2jpg.
    fast_fails = true;
    fb = camera_frame_get(); assert(fb && fb->len == 5 && fb->buf[3] == 1 && slow_uses > 0); camera_frame_return(fb);

    // Both failing, or no driver frame: nothing returned, nothing leaked.
    slow_fails = true; settle();
    for (int i = 0; i < 3; ++i) assert(camera_frame_get() == nullptr);
    fast_fails = slow_fails = false; absent = true; settle();
    assert(camera_frame_get() == nullptr);
    absent = false; settle(); assert(outstanding == 0);

    // Paused (camera reconfiguring): ready frames are dropped and the driver
    // is left alone; nesting is allowed. Frames flow again after resume.
    fb = camera_frame_get(); assert(fb); camera_frame_return(fb);
    camera_frames_pause(); camera_frames_pause(); paused = true;
    assert(outstanding == 0);
    assert(camera_frame_get() == nullptr);
    paused = false; camera_frames_resume(); camera_frames_resume();
    fb = camera_frame_get(); assert(fb && fb->len == 4); camera_frame_return(fb);
    settle(); assert(outstanding == 0);
    std::puts("ok"); std::fflush(stdout);
    std::_Exit(0);  // The encoder thread runs forever, as on the device.
}
'''


class CameraFrameTests(unittest.TestCase):
    def test_passthrough_pipeline_fallback_and_pause(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'freertos').mkdir()
            for name, text in FAKES.items():
                (root / name).write_text(text)
            (root / 'test.cpp').write_text(TEST)
            build = subprocess.run(['c++', '-std=c++17', '-fsanitize=address', '-g', '-pthread',
                                    '-I', str(root), '-I', str(ROOT / 'projects/webrtc/main'),
                                    str(root / 'test.cpp'), str(ROOT / 'projects/webrtc/main/camera_frames.cpp'),
                                    '-o', str(root / 'test')], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(root / 'test')], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('ok', run.stdout)


if __name__ == '__main__':
    unittest.main()
