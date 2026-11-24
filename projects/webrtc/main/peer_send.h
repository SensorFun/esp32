#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// Count of Wi-Fi TX queue-full results from the UDP transport (see peer_send.c).
uint32_t camera_transport_queue_full(void);
// Datagrams sent, payload bytes, time inside esp_peer's sendto (us), calls of
// 15 ms or more (an ENOBUFS sleep and retry), the longest call, and time in
// the shim's own queue-full retries. Reading resets the counters.
typedef struct {
    uint32_t calls, bytes, slow;
    int64_t us, max_us, retry_us;
} camera_udp_stats_t;
void camera_transport_udp_stats(camera_udp_stats_t *stats);
#ifdef __cplusplus
}
#endif
