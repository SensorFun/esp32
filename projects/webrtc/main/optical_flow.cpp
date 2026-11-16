#include "optical_flow.h"
#include <algorithm>
#include <cmath>

namespace motion {
namespace {

constexpr float kMaxFlow = 8.0f;

void box_blur(const float *in, float *out, float *tmp, int w, int h, int radius, Cooperate cooperate) {
    if (radius <= 0) { std::copy(in, in + w * h, out); return; }
    for (int y = 0; y < h; ++y) {
        if (cooperate && y % 8 == 0) cooperate();
        for (int x = 0; x < w; ++x) {
            float sum = 0;
            for (int k = -radius; k <= radius; ++k) sum += in[y * w + std::clamp(x + k, 0, w - 1)];
            tmp[y * w + x] = sum / (2 * radius + 1);
        }
    }
    for (int y = 0; y < h; ++y) {
        if (cooperate && y % 8 == 0) cooperate();
        for (int x = 0; x < w; ++x) {
            float sum = 0;
            for (int k = -radius; k <= radius; ++k) sum += tmp[std::clamp(y + k, 0, h - 1) * w + x];
            out[y * w + x] = sum / (2 * radius + 1);
        }
    }
}

inline float at(const float *image, int w, int h, int x, int y) {
    return image[std::clamp(y, 0, h - 1) * w + std::clamp(x, 0, w - 1)];
}

float bilinear(const float *image, int w, int h, float x, float y) {
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = x - x0, fy = y - y0;
    const float top = at(image, w, h, x0, y0) * (1 - fx) + at(image, w, h, x0 + 1, y0) * fx;
    const float bottom = at(image, w, h, x0, y0 + 1) * (1 - fx) + at(image, w, h, x0 + 1, y0 + 1) * fx;
    return top * (1 - fy) + bottom * fy;
}

float mean(const float *image, int count) {
    double sum = 0;
    for (int i = 0; i < count; ++i) sum += image[i];
    return float(sum / count);
}

struct Track { float u, v; bool textured; };

// Symmetric Lucas-Kanade for one window: gradients average both frames, so an
// object entering a flat background is still trackable. dmean removes global
// brightness changes such as auto exposure steps.
Track track(const float *prev, const float *cur, int w, int h, int x0, int y0, int x1, int y1,
            float u, float v, float dmean, float min_texture) {
    float gxx = 0, gxy = 0, gyy = 0;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float gx = 0.25f * (at(prev, w, h, x + 1, y) - at(prev, w, h, x - 1, y) + at(cur, w, h, x + 1, y) - at(cur, w, h, x - 1, y));
            const float gy = 0.25f * (at(prev, w, h, x, y + 1) - at(prev, w, h, x, y - 1) + at(cur, w, h, x, y + 1) - at(cur, w, h, x, y - 1));
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
                const float gx = 0.25f * (at(prev, w, h, x + 1, y) - at(prev, w, h, x - 1, y) + at(cur, w, h, x + 1, y) - at(cur, w, h, x - 1, y));
                const float gy = 0.25f * (at(prev, w, h, x, y + 1) - at(prev, w, h, x, y - 1) + at(cur, w, h, x, y + 1) - at(cur, w, h, x, y - 1));
                const float error = bilinear(cur, w, h, x + u, y + v) - at(prev, w, h, x, y) - dmean;
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
        const int sy0 = y * height / kHeight, sy1 = std::max(sy0 + 1, (y + 1) * height / kHeight);
        for (int x = 0; x < kWidth; ++x) {
            const int sx0 = x * width / kWidth, sx1 = std::max(sx0 + 1, (x + 1) * width / kWidth);
            uint32_t sum = 0, count = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint8_t *p = rgb + (size_t(sy) * width + sx0) * 3;
                for (int sx = sx0; sx < sx1; ++sx, p += 3) { sum += 77u * p[0] + 150u * p[1] + 29u * p[2]; ++count; }
            }
            gray[y * kWidth + x] = uint8_t(sum / (count * 256u));
        }
    }
}

bool valid_cell(int cell) { return cell >= 4 && kWidth % cell == 0 && kHeight % cell == 0; }

OpticalFlow::OpticalFlow()
    : prev_(kWidth * kHeight), cur_(kWidth * kHeight), scratch_(kWidth * kHeight * 2) {}

bool OpticalFlow::process(const uint8_t *gray, const FlowParams &params, FlowResult &result, Cooperate cooperate) {
    const int cell = valid_cell(params.cell) ? params.cell : 8;
    float *raw = scratch_.data(), *tmp = scratch_.data() + kWidth * kHeight;
    for (int i = 0; i < kWidth * kHeight; ++i) raw[i] = gray[i];
    box_blur(raw, cur_.data(), tmp, kWidth, kHeight, std::clamp(params.blur, 0, 2), cooperate);
    result.cols = kWidth / cell; result.rows = kHeight / cell;
    result.moving.assign(size_t(result.cols) * result.rows, 0);
    result.motion = false; result.area = 0; result.mean_flow = 0;
    if (!primed_) {
        std::swap(prev_, cur_);
        primed_ = true;
        return false;
    }
    const float dmean = mean(cur_.data(), kWidth * kHeight) - mean(prev_.data(), kWidth * kHeight);
    int moving = 0;
    float flow_sum = 0;
    for (int row = 0; row < result.rows; ++row) {
        if (cooperate) cooperate();
        for (int col = 0; col < result.cols; ++col) {
            const int x0 = col * cell, y0 = row * cell;
            // One level is enough at 160x120: detection only needs flow above
            // min_flow, and large motions still exceed it when underestimated.
            Track fine = track(prev_.data(), cur_.data(), kWidth, kHeight, x0, y0, x0 + cell, y0 + cell, 0, 0, dmean, params.min_texture);
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
