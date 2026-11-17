#include "optical_flow.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace motion {
namespace {

constexpr float kMaxFlow = 8.0f;

// Source span [first, end) that output index i averages along one axis.
inline int span_first(int i, int source, int output) { return i * source / output; }
inline int span_end(int i, int source, int output) {
    return std::max(span_first(i, source, output) + 1, (i + 1) * source / output);
}

inline uint8_t luma(const uint8_t *p) { return uint8_t((77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8); }

// Separable box blur in place; one line of scratch per pass.
void box_blur(uint8_t *image, int radius, Cooperate cooperate) {
    if (radius <= 0) return;
    const int taps = 2 * radius + 1;
    uint8_t line[kWidth > kHeight ? kWidth : kHeight];
    for (int y = 0; y < kHeight; ++y) {
        if (cooperate && y % 16 == 0) cooperate();
        uint8_t *row = image + y * kWidth;
        std::memcpy(line, row, kWidth);
        for (int x = 0; x < kWidth; ++x) {
            int sum = 0;
            for (int k = -radius; k <= radius; ++k) sum += line[std::clamp(x + k, 0, kWidth - 1)];
            row[x] = uint8_t((sum + taps / 2) / taps);
        }
    }
    for (int x = 0; x < kWidth; ++x) {
        if (cooperate && x % 32 == 0) cooperate();
        for (int y = 0; y < kHeight; ++y) line[y] = image[y * kWidth + x];
        for (int y = 0; y < kHeight; ++y) {
            int sum = 0;
            for (int k = -radius; k <= radius; ++k) sum += line[std::clamp(y + k, 0, kHeight - 1)];
            image[y * kWidth + x] = uint8_t((sum + taps / 2) / taps);
        }
    }
}

inline float at(const uint8_t *image, int x, int y) {
    return image[std::clamp(y, 0, kHeight - 1) * kWidth + std::clamp(x, 0, kWidth - 1)];
}

float bilinear(const uint8_t *image, float x, float y) {
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    const float top = at(image, x0, y0) * (1 - fx) + at(image, x0 + 1, y0) * fx;
    const float bottom = at(image, x0, y0 + 1) * (1 - fx) + at(image, x0 + 1, y0 + 1) * fx;
    return top * (1 - fy) + bottom * fy;
}

float mean(const uint8_t *image) {
    uint32_t sum = 0;
    for (int i = 0; i < kPixels; ++i) sum += image[i];
    return float(sum) / kPixels;
}

struct Track { float u, v; bool textured; };

// Symmetric Lucas-Kanade for one window: gradients average both frames, so an
// object entering a flat background is still trackable. dmean removes global
// brightness changes such as auto exposure steps.
Track track(const uint8_t *prev, const uint8_t *cur, int x0, int y0, int x1, int y1,
            float u, float v, float dmean, float min_texture) {
    float gxx = 0, gxy = 0, gyy = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float gx = 0.25f * (at(prev, x + 1, y) - at(prev, x - 1, y) + at(cur, x + 1, y) - at(cur, x - 1, y));
            const float gy = 0.25f * (at(prev, x, y + 1) - at(prev, x, y - 1) + at(cur, x, y + 1) - at(cur, x, y - 1));
            gxx += gx * gx; gxy += gx * gy; gyy += gy * gy; ++n;
        }
    }
    const float det = gxx * gyy - gxy * gxy;
    const float trace = gxx + gyy;
    const float eigen = 0.5f * (trace - std::sqrt(std::max(0.0f, trace * trace - 4 * det))) / std::max(n, 1);
    if (n == 0 || eigen < min_texture || det <= 1e-6f) return {u, v, false};
    for (int iteration = 0; iteration < 4; ++iteration) {
        float bx = 0, by = 0;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const float gx = 0.25f * (at(prev, x + 1, y) - at(prev, x - 1, y) + at(cur, x + 1, y) - at(cur, x - 1, y));
                const float gy = 0.25f * (at(prev, x, y + 1) - at(prev, x, y - 1) + at(cur, x, y + 1) - at(cur, x, y - 1));
                const float error = bilinear(cur, x + u, y + v) - at(prev, x, y) - dmean;
                bx += gx * error; by += gy * error;
            }
        }
        const float du = -(gyy * bx - gxy * by) / det;
        const float dv = -(gxx * by - gxy * bx) / det;
        u = std::clamp(u + du, -kMaxFlow, kMaxFlow);
        v = std::clamp(v + dv, -kMaxFlow, kMaxFlow);
        if (du * du + dv * dv < 1e-4f) break;
    }
    return {u, v, true};
}

}  // namespace

void rgb888_to_gray(const uint8_t *rgb, int width, int height, uint8_t *gray, Cooperate cooperate) {
    for (int y = 0; y < kHeight; ++y) {
        if (cooperate && y % 8 == 0) cooperate();
        const int sy0 = span_first(y, height, kHeight), sy1 = span_end(y, height, kHeight);
        for (int x = 0; x < kWidth; ++x) {
            const int sx0 = span_first(x, width, kWidth), sx1 = span_end(x, width, kWidth);
            uint32_t sum = 0;
            for (int sy = sy0; sy < sy1; ++sy)
                for (int sx = sx0; sx < sx1; ++sx) sum += luma(rgb + (size_t(sy) * width + sx) * 3);
            gray[y * kWidth + x] = uint8_t(sum / uint32_t((sx1 - sx0) * (sy1 - sy0)));
        }
    }
}

bool GrayScaler::begin(int width, int height, uint8_t *gray) {
    ok_ = width > 0 && height > 0 && width <= kMaxSource && height <= kMaxSource && gray;
    if (!ok_) return false;
    width_ = width; height_ = height; gray_ = gray; next_row_ = 0;
    std::memset(sums_, 0, sizeof(sums_));
    // Output columns that each source column contributes to (one, or two when upscaling).
    for (int sx = 0; sx < width; ++sx) { first_x_[sx] = UINT8_MAX; last_x_[sx] = 0; }
    for (int x = 0; x < kWidth; ++x)
        for (int sx = span_first(x, width, kWidth); sx < span_end(x, width, kWidth); ++sx) {
            first_x_[sx] = uint8_t(std::min<int>(first_x_[sx], x));
            last_x_[sx] = uint8_t(std::max<int>(last_x_[sx], x));
        }
    return true;
}

bool GrayScaler::add(const uint8_t *rgb, int left, int top, int right, int bottom, int stride) {
    if (!ok_ || left < 0 || top < 0 || right >= width_ || bottom >= height_ || left > right || top > bottom) return ok_ = false;
    const int columns = stride > 0 ? stride : right - left + 1;
    if (columns < right - left + 1) return ok_ = false;
    for (int sy = top; sy <= bottom; ++sy) {
        // Output rows whose span contains source row sy.
        int y = std::max(next_row_, span_first(sy, kHeight, height_) - 1);
        while (y < kHeight && span_first(y, height_, kHeight) <= sy) {
            if (sy < span_end(y, height_, kHeight)) {
                if (y >= next_row_ + kRing) return ok_ = false;
                uint16_t *sums = sums_[y % kRing];
                const uint8_t *p = rgb + size_t(sy - top) * columns * 3;
                for (int sx = left; sx <= right; ++sx, p += 3) {
                    const uint8_t value = luma(p);
                    for (int x = first_x_[sx]; x <= last_x_[sx]; ++x) sums[x] += value;
                }
            }
            ++y;
        }
    }
    // A block ending at the right edge completes every source row up to bottom.
    if (right == width_ - 1) close_rows(bottom);
    return ok_;
}

void GrayScaler::close_rows(int last_source_row) {
    while (next_row_ < kHeight && span_end(next_row_, height_, kHeight) - 1 <= last_source_row) {
        const int y = next_row_;
        const int rows = span_end(y, height_, kHeight) - span_first(y, height_, kHeight);
        uint16_t *sums = sums_[y % kRing];
        for (int x = 0; x < kWidth; ++x) {
            const int count = rows * (span_end(x, width_, kWidth) - span_first(x, width_, kWidth));
            gray_[y * kWidth + x] = uint8_t(sums[x] / count);
            sums[x] = 0;
        }
        ++next_row_;
    }
}

bool valid_cell(int cell) { return cell >= 4 && kWidth % cell == 0 && kHeight % cell == 0; }

OpticalFlow::OpticalFlow() : own_(size_t(kPixels) * 2), prev_(own_.data()), cur_(own_.data() + kPixels) {}

bool OpticalFlow::process(const uint8_t *gray, const FlowParams &params, FlowResult &result, Cooperate cooperate) {
    std::memcpy(cur_, gray, kPixels);
    return process(params, result, cooperate);
}

bool OpticalFlow::process(const FlowParams &params, FlowResult &result, Cooperate cooperate) {
    const int cell = valid_cell(params.cell) ? params.cell : 8;
    box_blur(cur_, std::clamp(params.blur, 0, 2), cooperate);
    result.cols = kWidth / cell; result.rows = kHeight / cell;
    result.moving.assign(size_t(result.cols) * result.rows, 0);
    result.motion = false; result.area = 0; result.mean_flow = 0;
    if (!primed_) {
        std::swap(prev_, cur_);
        primed_ = true;
        return false;
    }
    const float dmean = mean(cur_) - mean(prev_);
    int moving = 0;
    float flow_sum = 0;
    for (int row = 0; row < result.rows; ++row) {
        if (cooperate) cooperate();
        for (int col = 0; col < result.cols; ++col) {
            const int x0 = col * cell, y0 = row * cell;
            // One level is enough at 160x120: detection only needs flow above
            // min_flow, and large motions still exceed it when underestimated.
            Track fine = track(prev_, cur_, x0, y0, x0 + cell, y0 + cell, 0, 0, dmean, params.min_texture);
            if (!fine.textured) continue;
            const float magnitude = std::sqrt(fine.u * fine.u + fine.v * fine.v);
            if (magnitude >= params.min_flow) {
                result.moving[size_t(row) * result.cols + col] = 1;
                ++moving; flow_sum += magnitude;
            }
        }
    }
    result.area = 100.0f * moving / (result.cols * result.rows);
    result.mean_flow = moving ? flow_sum / moving : 0;
    result.motion = moving > 0 && result.area >= params.min_area;
    std::swap(prev_, cur_);
    return true;
}

}  // namespace motion
