#pragma once

#include "motion_settings.h"
#include "optical_flow.h"
#include <algorithm>
#include <cstdint>

namespace motion {

// Analysis runs as fast as the camera and CPU allow, so its rate varies with
// resolution and load. A miss shorter than `gap` (2.5 analysis intervals,
// 1-5 s) does not end continuous motion.
struct Activity {
    bool moving = false;
    int64_t since = 0, last = 0;
    int64_t gap = 1000000, previous = 0;  // previous: time of the last analysed frame.
    void update(bool detected, int64_t now) {
        // Clamped so a long pause (a download, a mode switch) cannot join motion
        // from before and after it into one event.
        if (previous) gap = std::clamp<int64_t>((now - previous) * 5 / 2, 1000000, 5000000);
        previous = now;
        if (detected) {
            if (!moving || now - last > gap) since = now;
            moving = true;
            last = now;
        } else if (now - last > gap) moving = false;
    }
    bool confirmed(int64_t now, float seconds) const {
        return moving && now - since >= int64_t(seconds * 1000000);
    }
};

// Table Huffman decoding needs about 9 KB for 4:2:2 camera JPEGs; the rest
// leaves room for 4:2:0 and larger tables.
struct JpegWorkspace { alignas(void *) uint8_t bytes[10240]; };

struct JpegScratch {
    GrayScaler scaler;
    JpegWorkspace workspace;
};

enum class FrameResult { invalid, priming, analyzed };

class Detector {
public:
    Detector(uint8_t *previous, uint8_t *current, JpegScratch &scratch)
        : Detector(previous, current, scratch.scaler, scratch.workspace) {}
    Detector(uint8_t *previous, uint8_t *current, GrayScaler &scaler, JpegWorkspace &workspace)
        : flow_(previous, current), scaler_(scaler), workspace_(workspace) {}
    void reset();
    FrameResult analyze(const uint8_t *jpeg, size_t size, int64_t captured,
                        const MotionSettings &s, Cooperate cooperate = nullptr);
    FlowResult flow;
    float interval = 0, speed = 0;
    int analysis_width = 0, analysis_height = 0;
private:
    OpticalFlow flow_;
    GrayScaler &scaler_;
    JpegWorkspace &workspace_;
    int width_ = 0, height_ = 0;
    int64_t previous_time_ = 0;
    bool have_previous_ = false;
};

}  // namespace motion
