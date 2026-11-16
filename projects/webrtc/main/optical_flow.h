#pragma once
// Pure C++ motion analysis, free of ESP-IDF so host tests can compile it.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace motion {

// Detection always runs on this frame size, whatever the stream resolution.
constexpr int kWidth = 160;
constexpr int kHeight = 120;

// Box-average an interleaved 8-bit RGB image (any size) into kWidth x kHeight luma.
using Cooperate = void (*)();
void rgb888_to_gray(const uint8_t *rgb, int width, int height, uint8_t *gray, Cooperate cooperate = nullptr);

struct FlowParams {
    int cell = 8;              // Square analysis cell in 160x120 pixels; must divide 160 and 120.
    float min_flow = 0.5f;     // Pixels per analysed frame that count as moving.
    float min_texture = 4.0f;  // Minimum Lucas-Kanade eigenvalue per pixel; flat cells are skipped.
    int blur = 1;              // Box blur radius applied before gradients (0-2).
    float min_area = 2.0f;     // Percentage of cells that must move to report motion.
};

struct FlowResult {
    bool motion = false;
    float area = 0;           // Percentage of all cells moving.
    float mean_flow = 0;      // Mean flow magnitude of moving cells, pixels per frame.
    int cols = 0, rows = 0;
    std::vector<uint8_t> moving;  // cols * rows, 1 for moving cells.
};

bool valid_cell(int cell);

// Single-level Lucas-Kanade over a grid of cells at 160x120. The first frame
// only primes the detector and reports no motion.
class OpticalFlow {
public:
    OpticalFlow();
    // gray is kWidth x kHeight. Returns false while priming.
    // Optional scheduler hook runs between bounded blocks of analysis work.
    bool process(const uint8_t *gray, const FlowParams &params, FlowResult &result, Cooperate cooperate = nullptr);
    void reset() { primed_ = false; }

private:
    std::vector<float> prev_, cur_, scratch_;
    bool primed_ = false;
};

}  // namespace motion
