#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

namespace camera_http {
inline bool ipv4(const sockaddr_storage &address, uint32_t &result) {
    if (address.ss_family == AF_INET) {
        result = reinterpret_cast<const sockaddr_in &>(address).sin_addr.s_addr;
        return true;
    }
#if !defined(ESP_PLATFORM) || CONFIG_LWIP_IPV6
    if (address.ss_family == AF_INET6) {
        const auto *bytes = reinterpret_cast<const sockaddr_in6 &>(address).sin6_addr.s6_addr;
        constexpr unsigned char prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (std::memcmp(bytes, prefix, sizeof(prefix)) == 0) {
            std::memcpy(&result, bytes + 12, sizeof(result));
            return true;
        }
    }
#endif
    return false;
}
inline bool private_ipv4(uint32_t network_address) {
    const uint32_t address = ntohl(network_address);
    return (address >> 24) == 10 || (address >> 20) == 0xac1 ||
           (address >> 16) == 0xc0a8 || (address >> 16) == 0xa9fe;
}
inline bool local_peer(const sockaddr_storage &peer, uint32_t address, uint32_t mask) {
    uint32_t remote = 0;
    return mask && private_ipv4(address) && ipv4(peer, remote) && private_ipv4(remote) &&
           (remote & mask) == (address & mask);
}
inline bool origin_matches(const std::string &host, const std::string &origin,
                           const std::string &address, bool secure) {
    return (host == address || host == address + (secure ? ":443" : ":80")) &&
           (origin.empty() || origin == std::string(secure ? "https://" : "http://") + host);
}
#ifdef ESP_PLATFORM
inline bool allow_request(httpd_req_t *request, const esp_netif_ip_info_t &ip, bool secure) {
    const int socket = httpd_req_to_sockfd(request);
    sockaddr_storage local{}, peer{};
    socklen_t length = sizeof(local);
    uint32_t address = 0;
    if (getsockname(socket, reinterpret_cast<sockaddr *>(&local), &length) ||
        !ipv4(local, address) || address != ip.ip.addr) return false;
    if (!secure) {
        length = sizeof(peer);
        if (getpeername(socket, reinterpret_cast<sockaddr *>(&peer), &length) ||
            !local_peer(peer, address, ip.netmask.addr)) return false;
        // Do not trust forwarded addresses to grant plaintext access.
        for (const char *header : {"Forwarded", "X-Forwarded-For", "X-Forwarded-Host", "X-Forwarded-Proto"}) {
            if (httpd_req_get_hdr_value_len(request, header)) return false;
        }
    }
    char text[20]{}, host[48]{}, origin[64]{};
    snprintf(text, sizeof(text), IPSTR, IP2STR(&ip.ip));
    if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK) return false;
    if (httpd_req_get_hdr_value_len(request, "Origin") &&
        httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK) return false;
    return origin_matches(host, origin, text, secure);
}
#endif
}
