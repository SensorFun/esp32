"""Check JPEG passthrough and software encoding ownership with a fake driver."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CameraFrameTests(unittest.TestCase):
    def test_conversion_and_frame_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'esp_camera.h').write_text('''#pragma once
#include <cstddef>
#include <cstdint>
enum pixformat_t { PIXFORMAT_RGB565, PIXFORMAT_JPEG };
struct camera_fb_t { uint8_t *buf; size_t len, width, height; pixformat_t format; };
camera_fb_t *esp_camera_fb_get();
void esp_camera_fb_return(camera_fb_t *);
''')
            (root / 'img_converters.h').write_text('''#pragma once
#include "esp_camera.h"
bool frame2jpg(camera_fb_t *, uint8_t, uint8_t **, size_t *);
''')
            (root / 'test.cpp').write_text('''#include "camera_frames.h"
#include <cassert>
#include <cstdlib>
static uint8_t pixels[8]{};
static camera_fb_t raw{pixels, sizeof(pixels), 2, 2, PIXFORMAT_RGB565};
static bool absent, fail;
static int releases;
camera_fb_t *esp_camera_fb_get() { return absent ? nullptr : &raw; }
void esp_camera_fb_return(camera_fb_t *fb) { assert(fb == &raw); ++releases; }
bool frame2jpg(camera_fb_t *fb, uint8_t quality, uint8_t **data, size_t *length) {
    assert(fb == &raw && quality == 80);
    if (fail) return false;
    *data = static_cast<uint8_t *>(malloc(4));
    (*data)[0] = 0xff; (*data)[1] = 0xd8;
    *length = 4;
    return true;
}
int main() {
    absent = true; assert(camera_frame_get() == nullptr); assert(releases == 0);
    absent = false; raw.format = PIXFORMAT_JPEG;
    auto *fb = camera_frame_get(); assert(fb == &raw && releases == 0);
    camera_frame_return(fb); assert(releases == 1);
    raw.format = PIXFORMAT_RGB565;
    for (int i = 0; i < 40; ++i) {
        int before = releases;
        fb = camera_frame_get(); assert(fb && fb != &raw && fb->format == PIXFORMAT_JPEG);
        assert(fb->width == 2 && fb->height == 2 && fb->len == 4 && fb->buf[1] == 0xd8);
        assert(releases == before + 1);
        camera_frame_return(fb); assert(releases == before + 1);
    }
    fail = true; int before = releases;
    assert(camera_frame_get() == nullptr && releases == before + 1);
}
''')
            subprocess.run(['c++', '-std=c++17', '-fsanitize=address', '-g',
                            '-I', str(root), '-I', str(ROOT / 'projects/webrtc/main'),
                            str(root / 'test.cpp'), str(ROOT / 'projects/webrtc/main/camera_frames.cpp'),
                            '-o', str(root / 'test')], check=True, capture_output=True)
            subprocess.run([str(root / 'test')], check=True, capture_output=True)
