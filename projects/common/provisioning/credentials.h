#pragma once
#include <cctype>
#include <string_view>
#include "signaling_settings.h"

namespace provisioning {
// Limits are bytes, matching ESP-IDF's Wi-Fi configuration fields.
inline bool valid_credentials(std::string_view ssid, std::string_view password) {
    if (ssid.empty() || ssid.size() > 32 || ssid.find('\0') != ssid.npos ||
        password.find('\0') != password.npos) return false;
    if (password.empty()) return true; // Open network.
    if (password.size() >= 8 && password.size() <= 63) return true;
    if (password.size() != 64) return false;
    for (unsigned char ch : password) if (!std::isxdigit(ch)) return false;
    return true;
}
}

#include <cstring>
#include <string>

namespace provisioning {
struct Credentials {
    char ssid[33]{};
    char password[65]{};
};

inline int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

inline bool form_decode(std::string_view value, std::string &out) {
    out.clear();
    for (size_t i = 0; i < value.size(); ++i) {
        char ch = value[i];
        if (ch == '%') {
            if (i + 2 >= value.size()) return false;
            const int hi = hex_digit(value[i + 1]), lo = hex_digit(value[i + 2]);
            if (hi < 0 || lo < 0) return false;
            ch = static_cast<char>((hi << 4) | lo);
            i += 2;
        } else if (ch == '+') {
            ch = ' ';
        }
        if (ch == '\0') return false;
        out += ch;
    }
    return true;
}

inline bool parse_credentials(std::string_view body, Credentials &out, std::string *camera_password = nullptr, std::string *signaling_url = nullptr) {
    std::string ssid, password, signal_server, custom_url;
    bool have_ssid = false, have_password = false, have_camera = false, have_server = false, have_custom = false;
    if (signaling_url) signaling_url->clear();
    while (!body.empty()) {
        const size_t end = body.find('&');
        const auto pair = body.substr(0, end);
        const size_t equals = pair.find('=');
        if (equals == pair.npos) return false;
        const auto key = pair.substr(0, equals), value = pair.substr(equals + 1);
        if (key == "ssid" && !have_ssid) {
            if (!form_decode(value, ssid)) return false;
            have_ssid = true;
        } else if (key == "password" && !have_password) {
            if (!form_decode(value, password)) return false;
            have_password = true;
        } else if (key == "camera_password" && camera_password && !have_camera) {
            if (!form_decode(value, *camera_password)) return false;
            have_camera = true;
        } else if (key == "signal_server" && signaling_url && !have_server) {
            if (!form_decode(value, signal_server)) return false;
            have_server = true;
        } else if (key == "signal_server_url" && signaling_url && !have_custom) {
            if (!form_decode(value, custom_url)) return false;
            have_custom = true;
        } else return false;
        if (end == body.npos) break;
        body.remove_prefix(end + 1);
    }
    if (!have_ssid || !have_password || !valid_credentials(ssid, password)) return false;
    if (camera_password && (!have_camera || camera_password->size() < 12 || camera_password->size() > 96)) return false;
    if (have_custom && !have_server) return false;
    if (have_server) {
        if (signal_server == "builtin" && custom_url.empty()) *signaling_url = DEFAULT_SIGNALING_URL;
        else if (signal_server == "custom" && have_custom && valid_signaling_url(custom_url)) *signaling_url = custom_url;
        else return false;
    }
    out = {};
    std::memcpy(out.ssid, ssid.data(), ssid.size());
    std::memcpy(out.password, password.data(), password.size());
    return true;
}

inline std::string json_quote(std::string_view text) {
    const char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += ch; }
        else if (ch < 0x20) {
            result += "\\u00";
            result += hex[ch >> 4]; result += hex[ch & 15];
        } else result += ch;
    }
    result += '"';
    return result;
}
}
