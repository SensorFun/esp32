#pragma once
#include <cmath>
#include <cstddef>
#include <cstdio>

inline bool valid_janus_id(double value) {
    return std::isfinite(value) && value >= 1 && value <= 9007199254740991.0 && std::floor(value) == value;
}

inline bool format_janus_id(double value, char *out, size_t size) {
    if (!out || !size || !valid_janus_id(value)) return false;
    const int length = std::snprintf(out, size, "%.0f", value);
    return length > 0 && static_cast<size_t>(length) < size;
}
