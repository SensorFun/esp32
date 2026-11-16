#pragma once
// Motion-JPEG AVI files, written with POSIX calls only so host tests can run it.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace motion {

struct AviInfo {
    uint32_t frames = 0;
    uint32_t micro_per_frame = 0;
    uint16_t width = 0, height = 0;
    bool finalized = false;  // Header and index were written when the clip closed.
};

// Reads only the fixed-size header written by AviWriter.
bool read_avi_info(int fd, AviInfo &info);

class AviWriter {
public:
    // staging must stay valid until close(); it batches small writes into
    // DMA-capable memory on the device. Its size should be a multiple of 512.
    bool open(const char *path, uint16_t width, uint16_t height, uint8_t *staging, size_t staging_size);
    bool add_frame(const uint8_t *jpeg, size_t length);
    // Flushes buffered data and makes it durable without finalizing the file.
    bool sync();
    // Writes the index and the final header. duration_us is the wall time the
    // frames cover; the playback rate is frames / duration.
    bool close(uint64_t duration_us);
    // Closes without finalizing, for I/O errors.
    void abort();
    bool is_open() const { return fd_ >= 0; }
    uint32_t frames() const { return uint32_t(index_.size() / 2); }
    uint64_t bytes() const { return position_; }
    uint16_t width() const { return width_; }
    uint16_t height() const { return height_; }

private:
    bool put(const void *data, size_t length);
    bool flush();
    bool write_header(uint32_t micro_per_frame, bool final);
    int fd_ = -1;
    uint8_t *staging_ = nullptr;
    size_t staging_size_ = 0, staged_ = 0;
    uint64_t position_ = 0;
    uint32_t largest_ = 0;
    uint16_t width_ = 0, height_ = 0;
    std::vector<uint32_t> index_;  // Offset, size pairs relative to "movi".
};

}  // namespace motion
