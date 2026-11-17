// Host checks for the firmware's optical flow and AVI writer. Prints one JSON
// line per check; tests/test_motion_native.py compiles and runs this file.
#include "optical_flow.h"
#include "avi_writer.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

using namespace motion;

static float scene(float x, float y) {
    return 128 + 35 * std::sin(0.31f * x + 0.17f * y) + 30 * std::sin(0.23f * y - 0.11f * x) + 20 * std::sin(0.53f * x * 0.7f + 0.41f * y);
}

static std::vector<uint8_t> frame(float dx, float dy, float brightness = 0, int noise = 0, unsigned seed = 1) {
    std::vector<uint8_t> out(kWidth * kHeight);
    srand(seed);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            float v = scene(x - dx, y - dy) + brightness + (noise ? rand() % (2 * noise + 1) - noise : 0);
            out[y * kWidth + x] = uint8_t(v < 0 ? 0 : v > 255 ? 255 : v);
        }
    return out;
}

static unsigned cooperative_calls = 0;
static void cooperate() { ++cooperative_calls; }

static FlowResult run(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, FlowParams params = {}) {
    OpticalFlow flow;
    FlowResult result;
    flow.process(a.data(), params, result);
    flow.process(b.data(), params, result);
    OpticalFlow scheduled;
    FlowResult actual;
    cooperative_calls = 0;
    scheduled.process(a.data(), params, actual, cooperate);
    scheduled.process(b.data(), params, actual, cooperate);
    assert(cooperative_calls > 0);
    assert(actual.motion == result.motion && actual.area == result.area && actual.mean_flow == result.mean_flow);
    assert(actual.moving == result.moving);
    return result;
}

static void report(const char *name, const FlowResult &r) {
    printf("{\"check\":\"%s\",\"motion\":%s,\"area\":%.2f,\"flow\":%.3f,\"cols\":%d,\"rows\":%d}\n",
           name, r.motion ? "true" : "false", r.area, r.mean_flow, r.cols, r.rows);
}

int main(int argc, char **argv) {
    report("shift_1px", run(frame(0, 0), frame(1, 0)));
    report("shift_3px", run(frame(0, 0), frame(3, 2)));
    report("noise_only", run(frame(0, 0, 0, 2, 1), frame(0, 0, 0, 2, 2)));
    report("brightness", run(frame(0, 0), frame(0, 0, 25)));
    std::vector<uint8_t> flat(kWidth * kHeight, 90), flat2(kWidth * kHeight, 90);
    report("flat", run(flat, flat2));
    // A textured 24x24 object moves 3 px across a static scene.
    auto background = frame(0, 0), before = background, after = background;
    for (int y = 40; y < 64; ++y)
        for (int x = 60; x < 84; ++x) {
            before[y * kWidth + x] = uint8_t(((x / 3 + y / 3) & 1) ? 230 : 20);
            after[y * kWidth + x + 3] = uint8_t(((x / 3 + y / 3) & 1) ? 230 : 20);
        }
    for (int y = 40; y < 64; ++y) after[y * kWidth + 60] = after[y * kWidth + 61] = after[y * kWidth + 62] = background[y * kWidth + 60];
    FlowParams small; small.min_area = 1;
    report("object_small_area", run(before, after, small));
    FlowParams strict; strict.min_area = 50;
    report("object_strict_area", run(before, after, strict));
    FlowParams cells; cells.cell = 20;
    report("cell_20", run(frame(0, 0), frame(2, 0), cells));
    // A uniform RGB image stays uniform at the analysis size.
    std::vector<uint8_t> rgb(320 * 240 * 3, 200), gray(kWidth * kHeight);
    cooperative_calls = 0;
    rgb888_to_gray(rgb.data(), 320, 240, gray.data(), cooperate);
    assert(cooperative_calls > 0);
    printf("{\"check\":\"gray\",\"value\":%d,\"last\":%d}\n", gray[0], gray.back());
    // Decoded sizes the camera produces after JPEG downscaling, fed as MCU
    // blocks (width x height) in decoder order, must match the reference.
    const int sizes[][2] = {{160, 120}, {200, 150}, {320, 180}, {256, 192}, {240, 240}, {160, 160}, {96, 96}, {128, 128}, {200, 148}};
    const int mcus[][2] = {{16, 16}, {16, 8}, {8, 8}, {4, 4}, {2, 2}, {2, 1}, {1, 1}};
    int compared = 0, mismatched = 0;
    for (const auto &size : sizes) {
        const int w = size[0], h = size[1];
        std::vector<uint8_t> image(size_t(w) * h * 3), expected(kWidth * kHeight), actual(kWidth * kHeight, 0);
        uint32_t seed = uint32_t(w * 7919 + h);
        for (auto &value : image) { seed = seed * 1103515245u + 12345u; value = uint8_t(seed >> 16); }
        rgb888_to_gray(image.data(), w, h, expected.data());
        for (const auto &mcu : mcus) {
            static GrayScaler scaler;
            bool ok = scaler.begin(w, h, actual.data());
            std::vector<uint8_t> block;
            for (int top = 0; ok && top < h; top += mcu[1])
                for (int left = 0; ok && left < w; left += mcu[0]) {
                    const int right = std::min(left + mcu[0], w) - 1, bottom = std::min(top + mcu[1], h) - 1;
                    block.clear();
                    for (int y = top; y <= bottom; ++y)
                        block.insert(block.end(), image.begin() + (size_t(y) * w + left) * 3, image.begin() + (size_t(y) * w + right + 1) * 3);
                    ok = scaler.add(block.data(), left, top, right, bottom);
                }
            ++compared;
            if (!ok || !scaler.finish() || actual != expected) ++mismatched;
        }
    }
    GrayScaler oversized;
    printf("{\"check\":\"scaler\",\"compared\":%d,\"mismatched\":%d,\"oversized\":%s}\n", compared, mismatched,
           oversized.begin(GrayScaler::kMaxSource + 1, 240, gray.data()) ? "true" : "false");

    if (argc > 1) {
        alignas(4) static uint8_t staging[1024];
        AviWriter avi;
        const uint8_t a[] = {0xff, 0xd8, 1, 2, 3, 0xff, 0xd9};  // odd length: padded
        const uint8_t b[] = {0xff, 0xd8, 4, 5, 0xff, 0xd9};
        std::vector<uint8_t> big(3000, 7); big[0] = 0xff; big[1] = 0xd8;
        bool ok = avi.open(argv[1], 320, 240, staging, sizeof(staging)) && avi.add_frame(a, sizeof(a)) && avi.add_frame(b, sizeof(b)) && avi.sync() && avi.add_frame(big.data(), big.size()) && avi.close(300000);
        int fd = open(argv[1], O_RDONLY);
        AviInfo info;
        bool read = fd >= 0 && read_avi_info(fd, info);
        if (fd >= 0) close(fd);
        printf("{\"check\":\"avi\",\"ok\":%s,\"read\":%s,\"frames\":%u,\"micro\":%u,\"width\":%u,\"height\":%u,\"finalized\":%s}\n",
               ok ? "true" : "false", read ? "true" : "false", info.frames, info.micro_per_frame, info.width, info.height, info.finalized ? "true" : "false");
    }
    return 0;
}
