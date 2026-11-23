#pragma once

#include "motion_settings.h"
#include "optical_flow.h"
#include <cstdint>

namespace motion {

class FrameSchedule {
public:
    bool due(int64_t now, bool busy) const { return !busy && now >= next_; }
    void admit(int64_t now, const MotionSettings &s) { next_ = now + 1000000 / s.analysis_fps; }
    void reset() { next_ = 0; }
private:
    int64_t next_ = 0;
};

struct Activity {
    bool moving = false;
    int64_t since = 0, last = 0;
    void update(bool detected, int64_t now, const MotionSettings &s);
    bool confirmed(int64_t now, float seconds) const {
        return moving && now - since >= int64_t(seconds * 1000000);
    }
};

struct JpegScratch {
    GrayScaler scaler;
    alignas(void *) uint8_t workspace[4096];
};

enum class FrameResult { invalid, priming, analyzed };

class Detector {
public:
    Detector(uint8_t *previous, uint8_t *current, JpegScratch &scratch)
        : flow_(previous, current), scratch_(scratch) {}
    void reset();
    FrameResult analyze(const uint8_t *jpeg, size_t size, int64_t captured,
                        const MotionSettings &s, Cooperate cooperate = nullptr);
    FlowResult flow;
    float interval = 0, speed = 0;
    int analysis_width = 0, analysis_height = 0;
private:
    OpticalFlow flow_;
    JpegScratch &scratch_;
    int width_ = 0, height_ = 0;
    int64_t previous_time_ = 0;
    bool have_previous_ = false;
};

}  // namespace motion
