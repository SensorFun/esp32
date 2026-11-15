#include "camera_frames.h"
#include "img_converters.h"
#include <cstdlib>

// The worker owns at most one frame. Raw GC frames are returned to the driver
// immediately after conversion; JPEG memory lives until the worker releases it.
static camera_fb_t converted_frame{};
camera_fb_t *camera_frame_get() {
    camera_fb_t *raw = esp_camera_fb_get();
    if (!raw || raw->format == PIXFORMAT_JPEG) return raw;
    uint8_t *jpeg = nullptr;
    size_t length = 0;
    const bool ok = frame2jpg(raw, 80, &jpeg, &length);
    converted_frame = *raw;
    esp_camera_fb_return(raw);
    if (!ok) { free(jpeg); return nullptr; }
    converted_frame.buf = jpeg;
    converted_frame.len = length;
    converted_frame.format = PIXFORMAT_JPEG;
    return &converted_frame;
}
void camera_frame_return(camera_fb_t *frame) {
    if (frame == &converted_frame) { free(frame->buf); frame->buf = nullptr; }
    else esp_camera_fb_return(frame);
}
