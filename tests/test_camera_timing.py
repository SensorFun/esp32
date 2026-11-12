"""Exercise the firmware timing logic with sensor register faults and masks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CameraTimingTests(unittest.TestCase):
    def test_register_changes_restore_and_failures(self):
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler unavailable')
        stub = '''
#pragma once
#define OV2640_PID 0x26
#define OV3660_PID 0x3660
struct sensor_t {
    struct { int PID; } id;
    int (*get_reg)(sensor_t *, int, int);
    int (*set_reg)(sensor_t *, int, int, int);
};
'''
        source = r'''
#include "camera_timing.h"
#include <cassert>
#include <initializer_list>
static int clock_value, hts_value, fail_count;
static int read(sensor_t *, int reg, int mask) {
    return (reg == 0x111 ? clock_value : hts_value) & mask;
}
static int write(sensor_t *, int reg, int mask, int value) {
    if (fail_count > 0) { --fail_count; return -1; }
    int &target = reg == 0x111 ? clock_value : hts_value;
    target = (target & ~mask) | (value & mask);
    return 0;
}
int main() {
    for (int pid : {OV2640_PID, OV3660_PID}) {
        clock_value = 0x80; hts_value = 2300;
        sensor_t sensor{{pid}, read, write};
        CameraTiming timing;
        assert(timing.initialize(&sensor) == 0);
        int baseline = timing.readback();
        for (int profile : {0, 1, 2, 1, 0, 2, 0}) {
            assert(timing.apply(profile) == 0);
            int expected = pid == OV3660_PID ? baseline * (profile == 2 ? 3 : 1 << profile) : (baseline + 1) * (1 << profile) - 1;
            assert(timing.readback() == expected);
            assert(timing.selected() == profile);
            assert((clock_value & 0x80) == 0x80);
        }
        assert(timing.apply(-1) == -1);
        assert(timing.apply(3) == -1);
        fail_count = 1;
        assert(timing.apply(1) == -1);
        assert(timing.selected() == 0 && timing.readback() == baseline);
        fail_count = 2;
        assert(timing.apply(1) == -2);
    }
    hts_value = 7000;
    sensor_t overflow{{OV3660_PID}, read, write};
    CameraTiming timing;
    assert(timing.initialize(&overflow) == 0);
    assert(timing.apply(1) == -1);
    assert(timing.readback() == 7000);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'esp_camera.h').write_text(stub)
            (path / 'check.cpp').write_text(source)
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(path), '-I', str(ROOT / 'projects/webrtc/main'),
                            str(path / 'check.cpp'), '-o', str(path / 'check')],
                           check=True, capture_output=True)
            subprocess.run([str(path / 'check')], check=True)
