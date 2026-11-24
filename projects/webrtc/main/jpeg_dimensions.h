#pragma once
#include <cstddef>
#include <cstdint>

inline bool jpeg_matches_capture_size(const uint8_t *data, size_t size, unsigned width, unsigned height) {
    if (size < 2 || data[0] != 0xff || data[1] != 0xd8) return false;
    for (size_t i = 2; i < size;) {
        if (data[i++] != 0xff) return false;
        while (i < size && data[i] == 0xff) ++i;
        if (size - i < 3) return false;
        const uint8_t marker = data[i++];
        if (marker == 0xda || marker == 0xd9) return false;
        const size_t length = size_t(data[i]) << 8 | data[i + 1];
        if (length < 2 || length > size - i) return false;
        if (marker == 0xc0) {
            return length >= 8 && (unsigned(data[i + 3]) << 8 | data[i + 4]) == height &&
                   (unsigned(data[i + 5]) << 8 | data[i + 6]) == width;
        }
        i += length;
    }
    return false;
}
