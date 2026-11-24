#pragma once
#include "esp_camera.h"

// Preserve the driver's resolution-specific baseline. Longer line periods give
// longer integration time for the same exposure-line count. OV2640 uses CLKRC;
// OV3660 uses HTS, as the server's OV5647 implementation does.
class CameraTiming {
public:
    // Doubles the sensor clock. The driver rewrites the clock registers on every
    // mode change, so initialize() (called after each one) applies it again.
    // It applies up to `max_size` (YAML clock_2x.max_resolution); larger sizes
    // keep the driver's clock.
    // OV2640: CLKRC bit 7, the internal doubler; light timings change bits 5:0.
    // OV3660 (experimental): the driver's JPEG PLL divides by 3 before the VCO
    // (0x303D bits 5:4 = 3); /1.5 (1) doubles SYSCLK, and the pixel clock divider
    // (0x3824) is doubled first, so the ESP32 keeps receiving a 10 MHz pixel
    // clock. Other PLL settings, such as raw capture, are left alone.
    // Off leaves the driver's own values untouched.
    void set_clock_doubler(bool enabled, int max_size) { doubler_ = enabled; doubler_max_ = max_size; }
    // 1 when the sensor clock is doubled, 0 when not, -1 for other sensors.
    int clock_doubler() const {
        if (!sensor_) return -1;
        if (sensor_->id.PID == OV2640_PID) {
            const int value = sensor_->get_reg(sensor_, 0x111, 0x80);
            return value < 0 ? -1 : value != 0;
        }
        if (sensor_->id.PID == OV3660_PID) {
            const int value = sensor_->get_reg(sensor_, 0x303d, 0x30);
            return value < 0 ? -1 : value == 0x10;
        }
        return -1;
    }
    int initialize(sensor_t *sensor) {
        sensor_ = sensor;
        if (sensor_->id.PID == OV3660_PID) {
            if (doubling() && double_ov3660_pll()) return -1;
            baseline_ = sensor_->get_reg(sensor_, 0x380c, 0x1fff);
        }
        else if (sensor_->id.PID == OV2640_PID) {
            if (doubling() &&
                (sensor_->set_reg(sensor_, 0x111, 0x80, 0x80) || sensor_->get_reg(sensor_, 0x111, 0x80) != 0x80)) return -1;
            baseline_ = sensor_->get_reg(sensor_, 0x111, 0x3f);
        }
        else return -1;
        return baseline_ < 0 || (sensor_->id.PID == OV3660_PID && baseline_ == 0) ? -1 : 0;
    }
    int apply(int profile) {
        if (!sensor_ || profile < 0 || profile > 2) return -1;
        const int factor = sensor_->id.PID == OV3660_PID && profile == 2 ? 3 : 1 << profile;
        const bool ov3660 = sensor_->id.PID == OV3660_PID;
        const int reg = ov3660 ? 0x380c : 0x111;
        const int mask = ov3660 ? 0x1fff : 0x3f;
        const int value = ov3660 ? baseline_ * factor : (baseline_ + 1) * factor - 1;
        if (value > mask) return -1;
        const int previous = sensor_->get_reg(sensor_, reg, mask);
        if (previous < 0) return -1;
        if (sensor_->set_reg(sensor_, reg, mask, value) || sensor_->get_reg(sensor_, reg, mask) != value) {
            // Restore the previous timing if a multi-byte write/readback fails.
            if (sensor_->set_reg(sensor_, reg, mask, previous)) return -2;
            return -1;
        }
        selected_ = profile;
        return 0;
    }
    int selected() const { return selected_; }
    int readback() const {
        return sensor_->get_reg(sensor_, sensor_->id.PID == OV3660_PID ? 0x380c : 0x111,
                                sensor_->id.PID == OV3660_PID ? 0x1fff : 0x3f);
    }
private:
    bool doubling() const { return doubler_ && int(sensor_->status.framesize) <= doubler_max_; }
    int double_ov3660_pll() {
        const int pre = sensor_->get_reg(sensor_, 0x303d, 0x30);
        if (pre < 0) return -1;
        if (pre != 0x30) return 0;  // Already doubled, or not the driver's JPEG PLL.
        const int ratio = sensor_->get_reg(sensor_, 0x3824, 0x1f);
        if (ratio < 0) return -1;
        if (ratio == 0 || ratio * 2 > 0x1f) return 0;  // No divider room to keep the pixel clock.
        if (sensor_->set_reg(sensor_, 0x3824, 0x1f, ratio * 2) || sensor_->set_reg(sensor_, 0x303d, 0x30, 0x10)) return -1;
        return sensor_->get_reg(sensor_, 0x3824, 0x1f) == ratio * 2 && sensor_->get_reg(sensor_, 0x303d, 0x30) == 0x10 ? 0 : -1;
    }
    sensor_t *sensor_ = nullptr;
    int baseline_ = -1, selected_ = 0;
    bool doubler_ = false;
    int doubler_max_ = -1;
};
