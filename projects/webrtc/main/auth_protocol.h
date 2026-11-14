#pragma once
#include <string>
#include <cstddef>
#include <cstdint>
namespace access_protocol {
inline std::string hex(const uint8_t *data, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (size_t i = 0; i < size; ++i) { out[i * 2] = digits[data[i] >> 4]; out[i * 2 + 1] = digits[data[i] & 15]; }
    return out;
}
inline bool unhex(const std::string &text, uint8_t *out, size_t size) {
    if (text.size() != size * 2) return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; ++j) {
            char c = text[i * 2 + j];
            if (c >= 'A' && c <= 'F') c += 'a' - 'A';
            int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (digit < 0) return false;
            value = (value << 4) | digit;
        }
        out[i] = value;
    }
    return true;
}
inline std::string fingerprint(const std::string &sdp) {
    // This project negotiates one data-only media section. Reject duplicate or
    // inherited/overridden fingerprints so the authenticated fingerprint cannot
    // differ from the certificate selected by the SDP/DTLS implementation.
    std::string lower = sdp;
    for (char &c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    const size_t declaration = lower.find("a=fingerprint:");
    if (declaration == std::string::npos || lower.find("a=fingerprint:", declaration + 1) != std::string::npos) return {};
    const std::string prefix = "a=fingerprint:sha-256 ";
    const size_t start = sdp.find(prefix);
    if (start == std::string::npos || (start && sdp[start - 1] != '\n')) return {};
    const size_t end = sdp.find_first_of("\r\n", start);
    std::string result;
    for (size_t i = start + prefix.size(); i < sdp.size() && i != end; ++i) {
        char c = sdp[i]; if (c == ':') continue;
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return {};
        result += c;
    }
    return result.size() == 64 ? result : std::string{};
}
inline std::string transcript(const std::string &room, const std::string &salt, uint32_t iterations,
                              const std::string &nonce, const std::string &offer_fp, const std::string &answer_fp) {
    return "camera-scram-v2\n" + room + "\n" + salt + "\n" + std::to_string(iterations) + "\n" + nonce + "\n" + offer_fp + "\n" + answer_fp;
}
}
