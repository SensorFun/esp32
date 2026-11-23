#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Adaptive pacing for bulk data-channel traffic (video frames and recording
// downloads). Sending faster than the Wi-Fi link drains fills the SCTP send
// cache, the Wi-Fi TX queue and internal RAM, until DTLS acknowledgements and
// control replies cannot get out and the viewer drops. A fixed rate prevents
// that but wastes a strong link, so the rate follows the link instead
// (additive increase, multiplicative decrease, as in TCP congestion control):
//  - it rises by `step` every `increase_us` while traffic actually uses most of
//    the allowed rate (an idle or small-frame stream does not inflate it);
//  - any backpressure signal cuts it to 70%, at most once per `cooldown_us`,
//    so one congested burst does not collapse the rate.
// Times are microseconds, rates bytes per second.
class SendPacer {
public:
    struct Config {
        int64_t floor, start, ceiling, step;
        int64_t increase_us, cooldown_us;
        size_t chunk;  // Largest single send; unused credit allows 3 chunks.
    };
    explicit SendPacer(const Config &config) : config_(config) { reset(0); }

    void reset(int64_t now) {
        rate_ = config_.start;
        next_ = now;
        window_start_ = now;
        window_bytes_ = 0;
        last_cut_ = now - config_.cooldown_us;
        cuts_ = 0;
    }
    int64_t rate() const { return rate_; }
    uint32_t cuts() const { return cuts_; }
    bool due(int64_t now) const { return now >= next_; }

    // Accounts for bytes just handed to the transport.
    void sent(size_t bytes, int64_t now) {
        next_ = std::max(next_, now - burst_us()) + int64_t(bytes) * 1000000 / rate_;
        window_bytes_ += int64_t(bytes);
        if (now - window_start_ < config_.increase_us) return;
        const int64_t allowed = rate_ * (now - window_start_) / 1000000;
        if (window_bytes_ * 4 >= allowed * 3) rate_ = std::min(config_.ceiling, rate_ + config_.step);
        window_start_ = now;
        window_bytes_ = 0;
    }

    // Backpressure: a full send cache or TX queue, blocked control replies,
    // or internal RAM running low.
    void congested(int64_t now) {
        window_start_ = now;
        window_bytes_ = 0;
        if (now - last_cut_ < config_.cooldown_us) return;
        rate_ = std::max(config_.floor, rate_ * 7 / 10);
        last_cut_ = now;
        ++cuts_;
    }

private:
    // Unused credit a sender may spend at once: three chunks or 50 ms of
    // traffic, whichever is longer. A worker pass (receive wait, encryption,
    // capture) can take tens of milliseconds; credit it cannot bank is lost,
    // and the stream would then never use enough of the rate to raise it.
    int64_t burst_us() const {
        return std::max<int64_t>(50000, int64_t(config_.chunk) * 3 * 1000000 / rate_);
    }

    Config config_;
    int64_t rate_ = 0, next_ = 0, window_start_ = 0, window_bytes_ = 0, last_cut_ = 0;
    uint32_t cuts_ = 0;
};
