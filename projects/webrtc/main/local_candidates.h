#pragma once
#include <string>

// Direct LAN HTTPS already identifies the browser's reachable interface. Map
// browser-private mDNS host candidates to that HTTPS socket address;
// preserve the candidate UDP port, ICE credentials, and DTLS fingerprint.
// Other interfaces can still fail ICE checks; the reachable one wins normally.
inline unsigned normalize_local_candidates(std::string &sdp, const std::string &address) {
    unsigned changed = 0;
    size_t line = 0;
    while (line < sdp.size()) {
        size_t end = sdp.find('\n', line);
        if (end == std::string::npos) end = sdp.size();
        if (sdp.compare(line, 12, "a=candidate:") == 0) {
            size_t start = line;
            bool valid = true;
            for (int field = 0; field < 4; ++field) {
                start = sdp.find(' ', start);
                if (start == std::string::npos || start >= end) { valid = false; break; }
                start = sdp.find_first_not_of(' ', start);
                if (start == std::string::npos || start >= end) { valid = false; break; }
            }
            if (valid) {
                const size_t stop = sdp.find(' ', start);
                if (stop != std::string::npos && stop < end) {
                    std::string host = sdp.substr(start, stop - start);
                    for (auto &c : host) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                    if (!host.empty() && host.back() == '.') host.pop_back();
                    if (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0) {
                        sdp.replace(start, stop - start, address);
                        end = end - (stop - start) + address.size();
                        ++changed;
                    }
                }
            }
        }
        line = end + 1;
    }
    return changed;
}
