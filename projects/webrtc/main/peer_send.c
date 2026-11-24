#include "udp.h"
#include "peer_send.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>

extern int __real_udp_socket_sendto(udp_socket_t *, esp_peer_addr_t *, const uint8_t *, int);

// Times the Wi-Fi TX queue was full, even if a retry then succeeded: the
// earliest sign the link is saturated. The worker ends a bulk burst on it.
static volatile uint32_t queue_full_count;
uint32_t camera_transport_queue_full(void) { return queue_full_count; }

// UDP send timing, read and reset by the worker's stream timing log. esp_peer's
// own sendto retries an ENOBUFS (no free Wi-Fi TX buffer) after a 20 ms sleep;
// a retry that succeeds is not reported, so calls of 15 ms or more count it.
static camera_udp_stats_t udp_stats;
void camera_transport_udp_stats(camera_udp_stats_t *stats)
{
    *stats = udp_stats;
    udp_stats = (camera_udp_stats_t){0};
}

int __wrap_udp_socket_sendto(udp_socket_t *socket, esp_peer_addr_t *address,
                             const uint8_t *buffer, int length)
{
    const int64_t started = esp_timer_get_time();
    int result = __real_udp_socket_sendto(socket, address, buffer, length);
    const int64_t first = esp_timer_get_time() - started;
    ++udp_stats.calls;
    if (result > 0) udp_stats.bytes += (uint32_t)length;
    udp_stats.us += first;
    if (first >= 15000) ++udp_stats.slow;
    if (first > udp_stats.max_us) udp_stats.max_us = first;
    if (result == -200) queue_full_count = queue_full_count + 1;
    // The pinned UDP implementation returns -200 after its two short retries
    // exhaust the Wi-Fi TX queue. Retry the identical encrypted datagram here,
    // before returning to DTLS, so its pending record need not be abandoned.
    // Bound the wait; the peer loop must still service incoming traffic.
    while (result == -200 && esp_timer_get_time() - started < 250000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        result = __real_udp_socket_sendto(socket, address, buffer, length);
    }
    // Our own queue-full retries below, beyond esp_peer's.
    udp_stats.retry_us += esp_timer_get_time() - started - first;
    if (result < 0) {
        int error = errno;
        static int64_t last_report;
        int64_t now = esp_timer_get_time();
        if (now - last_report >= 1000000) {
            last_report = now;
            ESP_LOGW("camera_transport", "UDP send failed: result=%d errno=%d bytes=%d internal_free=%u largest=%u",
                     result, error, length,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        errno = error;
    }
    return result;
}
