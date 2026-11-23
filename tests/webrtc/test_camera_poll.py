"""Frame waits must yield to networking while retaining short/nonblocking calls."""
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_bounded_capture_wait(tmp_path):
    compiler = shutil.which('cc')
    if compiler is None:
        pytest.skip('C compiler unavailable')
    (tmp_path / 'freertos').mkdir()
    (tmp_path / 'freertos/FreeRTOS.h').write_text('typedef unsigned TickType_t;\n#define pdMS_TO_TICKS(ms) ((ms)/10)\n')
    (tmp_path / 'cam_hal.h').write_text('typedef struct {int unused;} camera_fb_t;')
    (tmp_path / 'check.c').write_text('''
#include "freertos/FreeRTOS.h"
#include "cam_hal.h"
#include <assert.h>
#include <stddef.h>
static camera_fb_t frame;
static TickType_t requested;
static camera_fb_t *result;
camera_fb_t *__real_cam_take(TickType_t timeout) { requested = timeout; return result; }
camera_fb_t *__wrap_cam_take(TickType_t timeout);
int main(void) {
    result = &frame;
    assert(__wrap_cam_take(pdMS_TO_TICKS(4000)) == &frame);
    assert(requested == pdMS_TO_TICKS(100));
    result = NULL;
    assert(__wrap_cam_take(pdMS_TO_TICKS(4000)) == NULL);
    assert(requested == pdMS_TO_TICKS(100));
    __wrap_cam_take(0); assert(requested == 0);
    __wrap_cam_take(1); assert(requested == 1);
    __wrap_cam_take(pdMS_TO_TICKS(100)); assert(requested == pdMS_TO_TICKS(100));
}
''')
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(tmp_path), str(ROOT / 'projects/webrtc/main/camera_poll.c'),
                    str(tmp_path / 'check.c'), '-o', str(tmp_path / 'check')], check=True)
    subprocess.run([str(tmp_path / 'check')], check=True)
