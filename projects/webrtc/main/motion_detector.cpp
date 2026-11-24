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
    uint8_t *direct = nullptr;
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
    const auto *rgb = static_cast<const uint8_t *>(pixels);
    const int stride = rect->right - rect->left + 1;
    if (!source.direct) return source.scaler.add(rgb, rect->left, rect->top, right, bottom, stride);
    for (int y = rect->top; y <= bottom; ++y) {
        const auto *pixel = rgb + (y - rect->top) * stride * 3;
        for (int x = rect->left; x <= right; ++x, pixel += 3)
            source.direct[y * source.width + x] = (77u * pixel[0] + 150u * pixel[1] + 29u * pixel[2]) >> 8;
    }
    return 1;
}
}  // namespace

void Detector::reset() {
    flow_.reset();
    have_previous_ = false;
}

FrameResult Detector::analyze(const uint8_t *jpeg, size_t size, int64_t captured,
                              const MotionSettings &s, Cooperate cooperate) {
    if (!s.enabled || s.method != 0) return FrameResult::invalid;
    Decode source{jpeg, size, scaler_};
    JDEC decoder{};
    if (jd_prepare(&decoder, input, workspace_.bytes, sizeof(workspace_.bytes), &source) != JDR_OK)
        return FrameResult::invalid;
    analysis_size(decoder.width, decoder.height, analysis_width, analysis_height);
    int scale = 0;
    while (scale < kMaxJpegScale &&
           ((decoder.width >> (scale + 1)) >= analysis_width && (decoder.height >> (scale + 1)) >= analysis_height))
        ++scale;
    while (scale < kMaxJpegScale &&
           std::max(decoder.width >> scale, decoder.height >> scale) > GrayScaler::kMaxSource)
        ++scale;
    source.width = std::max(1, decoder.width >> scale);
    source.height = std::max(1, decoder.height >> scale);
    if (source.width == analysis_width && source.height == analysis_height) source.direct = flow_.frame();
    else if (!scaler_.begin(source.width, source.height, analysis_width, analysis_height, flow_.frame()))
        return FrameResult::invalid;
    if (jd_decomp(&decoder, output, scale) != JDR_OK || (!source.direct && !scaler_.finish()))
        return FrameResult::invalid;
    if (decoder.width != width_ || decoder.height != height_) {
        reset();
        width_ = decoder.width;
        height_ = decoder.height;
    }
    interval = have_previous_ ? std::clamp(float(captured - previous_time_) / 1e6f, 0.02f, 5.0f) : 0.2f;
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
