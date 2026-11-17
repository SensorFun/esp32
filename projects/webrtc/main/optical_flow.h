#pragma once
// Pure C++ motion analysis, free of ESP-IDF so host tests can compile it.
// The working set is small enough for the ESP32's internal RAM: two 8-bit
// 160x120 frames plus a few KB of row sums.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace motion {

// Detection always runs on this frame size, whatever the stream resolution.
constexpr int kWidth = 160;
constexpr int kHeight = 120;
constexpr int kPixels = kWidth * kHeight;

using Cooperate = void (*)();

// Box-average an interleaved 8-bit RGB image (any size) into kWidth x kHeight luma.
// Reference for GrayScaler; both round identically.
void rgb888_to_gray(const uint8_t *rgb, int width, int height, uint8_t *gray, Cooperate cooperate = nullptr);

// The same box average, fed with decoded RGB888 blocks in raster MCU order, as
// a JPEG decoder emits them. Only a ring of partial row sums is kept, so the
// full decoded image never has to exist in memory.
class GrayScaler {
public:
    static constexpr int kMaxSource = 320;  // Widest and tallest decoded image accepted.
    static constexpr int kRing = 24;        // Output rows open at once: covers 16-row MCUs, upscaled.
    // Returns false when the decoded image is larger than kMaxSource.
    bool begin(int width, int height, uint8_t *gray);
    // One decoded block; rect bounds are inclusive and stride is the block's
    // row length in pixels (0: right - left + 1). Returns false on a layout the
    // ring cannot hold; the frame is then unusable.
    bool add(const uint8_t *rgb, int left, int top, int right, int bottom, int stride = 0);
    // True once every output row was completed.
    bool finish() const { return ok_ && next_row_ == kHeight; }

private:
    void close_rows(int last_source_row);
    uint16_t sums_[kRing][kWidth];
    uint8_t first_x_[kMaxSource], last_x_[kMaxSource];
    int width_ = 0, height_ = 0, next_row_ = 0;
    uint8_t *gray_ = nullptr;
    bool ok_ = false;
};

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
    // Uses two caller-owned kPixels buffers, e.g. in internal RAM.
    OpticalFlow(uint8_t *first, uint8_t *second) : prev_(first), cur_(second) {}
    // Allocates its own buffers (host tests).
    OpticalFlow();
    OpticalFlow(const OpticalFlow &) = delete;
    OpticalFlow &operator=(const OpticalFlow &) = delete;
    // Where the next kWidth x kHeight luma frame is written before process().
    uint8_t *frame() { return cur_; }
    // Analyses frame(); returns false while priming. The optional scheduler
    // hook runs between bounded blocks of work.
    bool process(const FlowParams &params, FlowResult &result, Cooperate cooperate = nullptr);
    // Copies gray (kWidth x kHeight) into frame() first.
    bool process(const uint8_t *gray, const FlowParams &params, FlowResult &result, Cooperate cooperate = nullptr);
    void reset() { primed_ = false; }

private:
    std::vector<uint8_t> own_;
    uint8_t *prev_, *cur_;
    bool primed_ = false;
};

}  // namespace motion
