"""Exercise the firmware timing logic with sensor register faults and masks."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CameraTimingTests(unittest.TestCase):
    def test_register_changes_restore_and_failures(self):
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler unavailable')
        stub = '''
#pragma once
#define OV2640_PID 0x26
#define OV3660_PID 0x3660
enum { FRAMESIZE_QVGA = 6, FRAMESIZE_SVGA = 11, FRAMESIZE_UXGA = 15 };
struct sensor_t {
    struct { int PID; } id;
    int (*get_reg)(sensor_t *, int, int);
    int (*set_reg)(sensor_t *, int, int, int);
    struct { int framesize; } status;
};
'''
        source = r'''
#include "camera_timing.h"
#include <cassert>
#include <initializer_list>
static int clock_value, hts_value, pll_value, ratio_value, fail_count;
static int &target(int reg) {
    return reg == 0x111 ? clock_value : reg == 0x303d ? pll_value : reg == 0x3824 ? ratio_value : hts_value;
}
static int read(sensor_t *, int reg, int mask) { return target(reg) & mask; }
static int write(sensor_t *, int reg, int mask, int value) {
    if (fail_count > 0) { --fail_count; return -1; }
    target(reg) = (target(reg) & ~mask) | (value & mask);
    return 0;
}
int main() {
    for (int pid : {OV2640_PID, OV3660_PID}) {
        clock_value = 0x80; hts_value = 2300;
        sensor_t sensor{{pid}, read, write, {}};
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
    // The OV2640 clock doubler: set by initialize() after the driver clears it,
    // preserved by every light timing, and left alone when disabled.
    clock_value = 0x01;
    sensor_t doubled{{OV2640_PID}, read, write, {}};
    CameraTiming doubler;
    doubler.set_clock_doubler(true, FRAMESIZE_SVGA);
    assert(doubler.initialize(&doubled) == 0);
    assert(clock_value == 0x81 && doubler.clock_doubler() == 1 && doubler.readback() == 0x01);
    for (int profile : {1, 2, 0}) { assert(doubler.apply(profile) == 0); assert((clock_value & 0x80) == 0x80); }
    clock_value = 0x00;  // a mode change rewrites CLKRC without the doubler
    assert(doubler.initialize(&doubled) == 0 && clock_value == 0x80);
    // UXGA sensor mode (above 800x600): the driver's clock is left alone.
    doubled.status.framesize = FRAMESIZE_UXGA;
    clock_value = 0x00;
    assert(doubler.initialize(&doubled) == 0 && clock_value == 0x00 && doubler.clock_doubler() == 0);
    doubled.status.framesize = FRAMESIZE_SVGA;
    assert(doubler.initialize(&doubled) == 0 && clock_value == 0x80);
    fail_count = 1;
    assert(doubler.initialize(&doubled) == -1);
    clock_value = 0x80;
    CameraTiming untouched;
    assert(untouched.initialize(&doubled) == 0 && clock_value == 0x80);
    clock_value = 0x00;
    assert(untouched.initialize(&doubled) == 0 && clock_value == 0x00 && untouched.clock_doubler() == 0);
    // OV3660: the driver's JPEG PLL (pre-divider /3, pixel divider 10) becomes
    // pre-divider /1.5 with pixel divider 20: double SYSCLK, same pixel clock.
    hts_value = 2300; pll_value = 0x30 | 0x02; ratio_value = 10;
    sensor_t ov3660{{OV3660_PID}, read, write, {}};
    CameraTiming pll;
    pll.set_clock_doubler(true, FRAMESIZE_SVGA);
    assert(pll.initialize(&ov3660) == 0);
    assert(pll_value == (0x10 | 0x02) && ratio_value == 20 && pll.clock_doubler() == 1);
    assert(pll.initialize(&ov3660) == 0 && pll_value == (0x10 | 0x02) && ratio_value == 20);  // idempotent
    assert(pll.apply(1) == 0 && pll.readback() == 4600 && pll_value == (0x10 | 0x02));
    pll_value = 0x30; ratio_value = 8;  // a mode change restores the driver PLL
    assert(pll.initialize(&ov3660) == 0 && pll_value == 0x10 && ratio_value == 16);
    pll_value = 0x00; ratio_value = 2;  // raw-capture PLL: left alone
    assert(pll.initialize(&ov3660) == 0 && pll_value == 0x00 && ratio_value == 2 && pll.clock_doubler() == 0);
    ov3660.status.framesize = FRAMESIZE_UXGA; pll_value = 0x30; ratio_value = 10;  // above the maximum: left alone
    assert(pll.initialize(&ov3660) == 0 && pll_value == 0x30 && ratio_value == 10 && pll.clock_doubler() == 0);
    ov3660.status.framesize = FRAMESIZE_SVGA;
    pll_value = 0x30; ratio_value = 20;  // no divider room: left alone
    assert(pll.initialize(&ov3660) == 0 && pll_value == 0x30 && ratio_value == 20);
    ratio_value = 10; fail_count = 1;
    assert(pll.initialize(&ov3660) == -1);
    CameraTiming plain;
    pll_value = 0x30; ratio_value = 10;
    assert(plain.initialize(&ov3660) == 0 && pll_value == 0x30 && ratio_value == 10 && plain.clock_doubler() == 0);
    hts_value = 7000;
    sensor_t overflow{{OV3660_PID}, read, write, {}};
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
