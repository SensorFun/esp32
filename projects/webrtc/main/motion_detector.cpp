#include "motion_detector.h"
#include "motion_jpeg.h"
#include <algorithm>
#include <cstring>

namespace motion {
namespace {
struct Decode {
    const uint8_t *jpeg;
    size_t size;
    GrayScaler &scaler;
    size_t offset = 0;
    int width = 0, height = 0;
};

size_t input(JDEC *decoder, uint8_t *buffer, size_t bytes) {
    auto &source = *static_cast<Decode *>(decoder->device);
    bytes = std::min(bytes, source.size - source.offset);
    if (buffer) memcpy(buffer, source.jpeg + source.offset, bytes);
    source.offset += bytes;
    return bytes;
}

int output(JDEC *decoder, void *pixels, JRECT *rect) {
    auto &source = *static_cast<Decode *>(decoder->device);
    const int right = std::min<int>(rect->right, source.width - 1);
    const int bottom = std::min<int>(rect->bottom, source.height - 1);
    if (rect->left > right || rect->top > bottom) return 1;
    return source.scaler.add(static_cast<const uint8_t *>(pixels),
        rect->left, rect->top, right, bottom, rect->right - rect->left + 1);
}
}  // namespace

void Activity::update(bool detected, int64_t now, const MotionSettings &s) {
    const int64_t gap = std::max<int64_t>(1000000, 2500000 / s.analysis_fps);
    if (detected) {
        if (!moving || now - last > gap) since = now;
        moving = true;
        last = now;
    } else if (now - last > gap) moving = false;
}

void Detector::reset() {
    flow_.reset();
    have_previous_ = false;
}

FrameResult Detector::analyze(const uint8_t *jpeg, size_t size, int64_t captured,
                              const MotionSettings &s, Cooperate cooperate) {
    if (!s.enabled || s.method != 0 || s.analysis_fps < 1) return FrameResult::invalid;
    Decode source{jpeg, size, scratch_.scaler};
    JDEC decoder{};
    if (jd_prepare(&decoder, input, scratch_.workspace, sizeof(scratch_.workspace), &source) != JDR_OK)
        return FrameResult::invalid;
    source.width = std::max(1, decoder.width >> kJpegScale);
    source.height = std::max(1, decoder.height >> kJpegScale);
    analysis_size(decoder.width, decoder.height, analysis_width, analysis_height);
    if (!source.scaler.begin(source.width, source.height, analysis_width, analysis_height, flow_.frame()) ||
        jd_decomp(&decoder, output, kJpegScale) != JDR_OK || !source.scaler.finish())
        return FrameResult::invalid;
    if (decoder.width != width_ || decoder.height != height_) {
        reset();
        width_ = decoder.width;
        height_ = decoder.height;
    }
    interval = have_previous_ ? std::clamp(float(captured - previous_time_) / 1e6f, 0.02f, 5.0f) : 1.0f / s.analysis_fps;
    previous_time_ = captured;
    have_previous_ = true;
    FlowParams params;
    params.cell = s.cell;
    params.min_flow = s.min_speed * interval;
    params.min_texture = s.min_texture;
    params.blur = s.blur;
    params.min_area = s.min_area;
    if (!flow_.process(analysis_width, analysis_height, params, flow, cooperate)) return FrameResult::priming;
    speed = flow.mean_flow / interval;
    return FrameResult::analyzed;
}
}  // namespace motion
