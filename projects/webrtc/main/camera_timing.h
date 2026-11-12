#pragma once
#include "esp_camera.h"

// Preserve the driver's resolution-specific baseline. Longer line periods give
// longer integration time for the same exposure-line count. OV2640 uses CLKRC;
// OV3660 uses HTS, as the server's OV5647 implementation does.
class CameraTiming {
public:
    int initialize(sensor_t *sensor) {
        sensor_ = sensor;
        if (sensor_->id.PID == OV3660_PID) baseline_ = sensor_->get_reg(sensor_, 0x380c, 0x1fff);
        else if (sensor_->id.PID == OV2640_PID) baseline_ = sensor_->get_reg(sensor_, 0x111, 0x3f);
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
    sensor_t *sensor_ = nullptr;
    int baseline_ = -1, selected_ = 0;
};
