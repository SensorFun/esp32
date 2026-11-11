#pragma once
#include <cstdint>

namespace common {
// Arm only after a continuous hold, then trigger once after release/debounce.
// Unsigned subtraction also handles timer wraparound.
class ResetHold {
public:
    explicit ResetHold(uint32_t hold_ms) : hold_ms_(hold_ms) {}
    bool update(bool active, uint32_t now_ms) {
        if (active) {
            releasing_ = false;
            if (!pressed_) { pressed_ = true; pressed_at_ = now_ms; }
            if (now_ms - pressed_at_ >= hold_ms_) armed_ = true;
        } else if (pressed_) {
            if (!releasing_) { releasing_ = true; released_at_ = now_ms; }
            if (now_ms - released_at_ >= 100) {
                const bool trigger = armed_;
                pressed_ = armed_ = releasing_ = false;
                return trigger;
            }
        }
        return false;
    }
    bool tracking() const { return pressed_; }
    bool armed() const { return armed_; }
private:
    uint32_t hold_ms_, pressed_at_ = 0, released_at_ = 0;
    bool pressed_ = false, armed_ = false, releasing_ = false;
};
}
