#pragma once

#include "motion_detector.h"
#include <algorithm>

namespace motion {

// A confirmed event keeps the sensor at the recording size through post-roll.
// The write queue is fed only after confirmation, including during live video.
class CaptureProfile {
public:
    bool update(int64_t now, const MotionSettings &settings, const Activity &activity, bool available) {
        const auto gap = activity.gap;
        const auto age = now - activity.last;
        if (!settings.enabled || !settings.record || !available ||
            age > std::max<int64_t>(gap, settings.post_seconds * 1000000LL)) {
            recording_ = false;
        } else if (age <= gap && activity.confirmed(now, settings.trigger_seconds)) {
            recording_ = true;
        }
        return recording_;
    }

private:
    bool recording_ = false;
};

}  // namespace motion
