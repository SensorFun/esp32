#pragma once
#include <cstdint>
#include <cstring>
#ifdef ESP_PLATFORM
#include "lwip/sockets.h"
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace provisioning {
// HTTPD uses dual-stack sockets when IPv6 is enabled, so an IPv4 client can
// appear as ::ffff:a.b.c.d. The expected address is in network byte order.
inline bool matches_setup_address(const sockaddr_storage &address, uint32_t expected) {
    if (address.ss_family == AF_INET) {
        const auto &ipv4 = reinterpret_cast<const sockaddr_in &>(address);
        return ipv4.sin_addr.s_addr == expected;
    }
#if !defined(ESP_PLATFORM) || CONFIG_LWIP_IPV6
    if (address.ss_family == AF_INET6) {
        const auto &ipv6 = reinterpret_cast<const sockaddr_in6 &>(address);
        constexpr unsigned char mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        const auto *bytes = ipv6.sin6_addr.s6_addr;
        return std::memcmp(bytes, mapped_prefix, sizeof(mapped_prefix)) == 0 &&
               std::memcmp(bytes + 12, &expected, sizeof(expected)) == 0;
    }
#endif
    return false;
}
}
