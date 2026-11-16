#include "avi_writer.h"
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace motion {
namespace {

// RIFF header (12) + hdrl LIST (12) + avih (64) + strl LIST (12) + strh (64)
// + strf (48) = 212, followed by the movi LIST header (12).
constexpr uint32_t kHeaderSize = 224;
constexpr uint32_t kMoviFourcc = 220;  // Index offsets are relative to "movi".

void put32(uint8_t *p, uint32_t value) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i)); }
void put16(uint8_t *p, uint16_t value) { p[0] = uint8_t(value); p[1] = uint8_t(value >> 8); }
uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

bool write_all(int fd, const uint8_t *data, size_t length) {
    while (length) {
        const ssize_t written = ::write(fd, data, length);
        if (written <= 0) return false;
        data += written; length -= size_t(written);
    }
    return true;
}

}  // namespace

bool read_avi_info(int fd, AviInfo &info) {
    uint8_t h[kHeaderSize];
    if (::lseek(fd, 0, SEEK_SET) != 0 || ::read(fd, h, sizeof(h)) != ssize_t(sizeof(h))) return false;
    if (memcmp(h, "RIFF", 4) || memcmp(h + 8, "AVI ", 4) || memcmp(h + 24, "avih", 4)) return false;
    info.micro_per_frame = get32(h + 32);
    info.frames = get32(h + 48);
    info.width = uint16_t(get32(h + 64));
    info.height = uint16_t(get32(h + 68));
    info.finalized = get32(h + 44) & 0x10;
    return true;
}

bool AviWriter::open(const char *path, uint16_t width, uint16_t height, uint8_t *staging, size_t staging_size) {
    abort();
    fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) return false;
    staging_ = staging; staging_size_ = staging_size; staged_ = 0;
    width_ = width; height_ = height; largest_ = 0; position_ = 0;
    index_.clear();
    index_.reserve(2 * 1024);
    if (!write_header(0, false)) { abort(); return false; }
    return true;
}

bool AviWriter::put(const void *data, size_t length) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    while (length) {
        const size_t room = std::min(length, staging_size_ - staged_);
        memcpy(staging_ + staged_, bytes, room);
        staged_ += room; bytes += room; length -= room; position_ += room;
        if (staged_ == staging_size_ && !flush()) return false;
    }
    return true;
}

bool AviWriter::flush() {
    if (!staged_) return true;
    const bool ok = write_all(fd_, staging_, staged_);
    staged_ = 0;
    return ok;
}

bool AviWriter::write_header(uint32_t micro_per_frame, bool final) {
    uint8_t h[kHeaderSize]{};
    const uint32_t frames = this->frames();
    const uint32_t movi_size = uint32_t(position_ > kHeaderSize ? position_ - kHeaderSize : 0) + 4;
    const uint32_t index_size = final ? frames * 16 : 0;
    memcpy(h, "RIFF", 4); put32(h + 4, kHeaderSize - 8 + movi_size - 4 + (final ? 8 + index_size : 0)); memcpy(h + 8, "AVI ", 4);
    memcpy(h + 12, "LIST", 4); put32(h + 16, 4 + 64 + 12 + 64 + 48); memcpy(h + 20, "hdrl", 4);
    memcpy(h + 24, "avih", 4); put32(h + 28, 56);
    put32(h + 32, micro_per_frame);
    put32(h + 36, micro_per_frame ? uint32_t(uint64_t(largest_) * 1000000 / micro_per_frame) : 0);
    put32(h + 44, final ? 0x10 : 0);  // AVIF_HASINDEX
    put32(h + 48, frames);
    put32(h + 56, 1);  // Streams
    put32(h + 60, largest_);
    put32(h + 64, width_); put32(h + 68, height_);
    memcpy(h + 88, "LIST", 4); put32(h + 92, 4 + 64 + 48); memcpy(h + 96, "strl", 4);
    memcpy(h + 100, "strh", 4); put32(h + 104, 56);
    memcpy(h + 108, "vids", 4); memcpy(h + 112, "MJPG", 4);
    put32(h + 128, micro_per_frame ? micro_per_frame : 1);  // Scale
    put32(h + 132, 1000000);                                 // Rate: rate / scale = frames per second
    put32(h + 140, frames);                                  // Length
    put32(h + 144, largest_);
    put32(h + 148, 0xffffffff);                              // Quality: default
    put16(h + 160, width_); put16(h + 162, height_);         // rcFrame right, bottom
    memcpy(h + 164, "strf", 4); put32(h + 168, 40);
    put32(h + 172, 40); put32(h + 176, width_); put32(h + 180, height_);
    put16(h + 184, 1); put16(h + 186, 24); memcpy(h + 188, "MJPG", 4);
    put32(h + 192, uint32_t(width_) * height_ * 3);
    memcpy(h + 212, "LIST", 4); put32(h + 216, movi_size); memcpy(h + 220, "movi", 4);
    if (position_ == 0) return put(h, sizeof(h));
    // Rewrite the header in place, then return to the end of the file.
    return ::lseek(fd_, 0, SEEK_SET) == 0 && write_all(fd_, h, sizeof(h)) && ::lseek(fd_, 0, SEEK_END) >= 0;
}

bool AviWriter::add_frame(const uint8_t *jpeg, size_t length) {
    if (fd_ < 0 || length == 0 || length > 0x7fffffff || position_ + length + 8 > 0xF0000000ull) return false;
    uint8_t chunk[8];
    memcpy(chunk, "00dc", 4); put32(chunk + 4, uint32_t(length));
    const uint64_t start = position_;
    static constexpr uint8_t pad = 0;
    if (!put(chunk, 8) || !put(jpeg, length) || ((length & 1) && !put(&pad, 1))) return false;
    index_.push_back(uint32_t(start - kMoviFourcc));
    index_.push_back(uint32_t(length));
    largest_ = std::max(largest_, uint32_t(length));
    return true;
}

bool AviWriter::sync() {
    return fd_ >= 0 && flush() && ::fsync(fd_) == 0;
}

bool AviWriter::close(uint64_t duration_us) {
    if (fd_ < 0) return false;
    bool ok = flush();
    const uint32_t frames = this->frames();
    const uint32_t micro = frames ? uint32_t(std::clamp<uint64_t>(duration_us / frames, 1000, 10000000)) : 100000;
    if (ok) {
        // idx1: every MJPEG frame is a key frame (AVIIF_KEYFRAME).
        uint8_t entry[16];
        memcpy(entry, "idx1", 4); put32(entry + 4, frames * 16);
        ok = put(entry, 8);
        for (uint32_t i = 0; ok && i < frames; ++i) {
            memcpy(entry, "00dc", 4); put32(entry + 4, 0x10); put32(entry + 8, index_[2 * i]); put32(entry + 12, index_[2 * i + 1]);
            ok = put(entry, 16);
        }
        // position_ now includes the index; the header's movi size must not.
        const uint64_t end = position_;
        position_ -= 8 + uint64_t(frames) * 16;
        ok = ok && flush() && write_header(micro, true);
        position_ = end;
    }
    ok = ::fsync(fd_) == 0 && ok;
    ok = ::close(fd_) == 0 && ok;
    fd_ = -1;
    index_.clear(); index_.shrink_to_fit();
    return ok;
}

void AviWriter::abort() {
    if (fd_ < 0) return;
    flush();
    ::close(fd_);
    fd_ = -1;
    index_.clear(); index_.shrink_to_fit();
}

}  // namespace motion
