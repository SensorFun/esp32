#pragma once
#include <string_view>

namespace provisioning {
inline constexpr char DEFAULT_SIGNALING_URL[] = "wss://signalserver.sensorfun.net/camera";
inline constexpr size_t SIGNALING_URL_SIZE = 257;

// Only secure WebSocket camera endpoints; never accept credentials, queries,
// fragments, control characters, or arbitrary paths in a provisioned URL.
inline bool valid_signaling_url(std::string_view url) {
    if (url.size() >= SIGNALING_URL_SIZE || url.substr(0, 6) != "wss://") return false;
    const auto endpoint = url.substr(6);
    const size_t slash = endpoint.find('/');
    if (slash == endpoint.npos || endpoint.substr(slash) != "/camera") return false;
    const auto authority = endpoint.substr(0, slash);
    const size_t colon = authority.find(':');
    const auto host = authority.substr(0, colon);
    if (host.empty() || host.front() == '.' || host.back() == '.' || host.front() == '-' || host.back() == '-') return false;
    for (char ch : host) if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-')) return false;
    if (colon != authority.npos) {
        const auto port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5) return false;
        unsigned number = 0;
        for (char ch : port) { if (ch < '0' || ch > '9') return false; number = number * 10 + unsigned(ch - '0'); }
        if (!number || number > 65535) return false;
    }
    return true;
}
}
