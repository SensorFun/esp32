#pragma once
// Pure C++ motion analysis, free of ESP-IDF so host tests can compile it.
// The working set is small enough for the ESP32's internal RAM: two 8-bit
// frames of at most kMaxPixels plus a few KB of row sums.
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace motion {

// Detection uses up to 160 × 120 luma pixels, regardless of capture size,
// keeping the camera's aspect ratio (160 × 90 at 16:9).
constexpr int kMaxJpegScale = 3;       // Decoder supports up to 1/8 scale.
constexpr int kMaxPixels = 160 * 120;  // Frame buffer capacity.

// Analysis size for a camera image of width x height.
void analysis_size(int width, int height, int &out_width, int &out_height);

using Cooperate = void (*)();

// Box-average an interleaved 8-bit RGB image (any size) into out_width x out_height luma.
// Reference for GrayScaler; both round identically.
void rgb888_to_gray(const uint8_t *rgb, int width, int height, uint8_t *gray, int out_width, int out_height,
                    Cooperate cooperate = nullptr);

// The same box average, fed with decoded blocks (RGB888 or luma) in raster MCU
// order, as a JPEG decoder emits them. Only a ring of partial row sums is kept, so the
// full decoded image never has to exist in memory.
class GrayScaler {
public:
    static constexpr int kMaxSource = 320;  // Widest and tallest decoded image accepted.
    static constexpr int kRing = 24;        // Output rows open at once: covers 16-row MCUs, upscaled.
    // Returns false when the decoded image is larger than kMaxSource or the
    // output is larger than kMaxSource or kMaxPixels.
    bool begin(int width, int height, int out_width, int out_height, uint8_t *gray);
    // One decoded block; rect bounds are inclusive and stride is the block's
    // row length in pixels (0: right - left + 1). channels is 3 for RGB888 or
    // 1 for luma. Returns false on a layout the ring cannot hold; the frame is
    // then unusable.
    bool add(const uint8_t *pixels, int left, int top, int right, int bottom, int stride = 0, int channels = 3);
    // True once every output row was completed.
    bool finish() const { return ok_ && next_row_ == out_height_; }

private:
    void close_rows(int last_source_row);
    uint16_t sums_[kRing][kMaxSource];
    uint16_t first_x_[kMaxSource], last_x_[kMaxSource];
    int width_ = 0, height_ = 0, out_width_ = 0, out_height_ = 0, next_row_ = 0;
    uint8_t *gray_ = nullptr;
    bool ok_ = false;
};

struct FlowParams {
    int cell = 8;              // Approximate square cell side in analysis pixels; cells tile the whole frame.
    float min_flow = 0.5f;     // Pixels per analysed frame that count as moving.
    float min_texture = 4.0f;  // Minimum structure-tensor eigenvalue per pixel; flat cells are skipped.
    int blur = 1;              // Box blur radius applied before gradients (0-2).
    float min_area = 2.0f;     // Percentage of cells that must move to report motion.
};

struct FlowResult {
    bool motion = false;
    float area = 0;           // Percentage of all cells moving.
    float mean_flow = 0;      // Mean flow magnitude of moving cells, pixels per frame.
    int cols = 0, rows = 0;
    std::array<uint8_t, kMaxPixels / 25> moving{};  // First cols * rows entries are valid.
};

bool valid_cell(int cell);

// Single-level optical flow over a grid of cells. The first frame of a given
// size only primes the detector and reports no motion.
class OpticalFlow {
public:
    // Uses two caller-owned kMaxPixels buffers, e.g. in internal RAM.
    OpticalFlow(uint8_t *first, uint8_t *second) : prev_(first), cur_(second) {}
    // Allocates its own buffers (host tests).
    OpticalFlow();
    OpticalFlow(const OpticalFlow &) = delete;
    OpticalFlow &operator=(const OpticalFlow &) = delete;
    // Where the next luma frame (row-major, width x height) is written before process().
    uint8_t *frame() { return cur_; }
    // Analyses frame(); returns false while priming or for an unusable size.
    // The optional scheduler hook runs between bounded blocks of work.
    bool process(int width, int height, const FlowParams &params, FlowResult &result, Cooperate cooperate = nullptr);
    // Copies gray (width x height) into frame() first.
    bool process(const uint8_t *gray, int width, int height, const FlowParams &params, FlowResult &result,
                 Cooperate cooperate = nullptr);
    void reset() { primed_ = false; }

private:
    std::vector<uint8_t> own_;
    uint8_t *prev_, *cur_;
    int width_ = 0, height_ = 0;
    bool primed_ = false;
};

}  // namespace motion
