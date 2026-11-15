#include "udp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>

extern int __real_udp_socket_sendto(udp_socket_t *, esp_peer_addr_t *, const uint8_t *, int);

int __wrap_udp_socket_sendto(udp_socket_t *socket, esp_peer_addr_t *address,
                             const uint8_t *buffer, int length)
{
    const int64_t started = esp_timer_get_time();
    int result = __real_udp_socket_sendto(socket, address, buffer, length);
    // The pinned UDP implementation returns -200 after its two short retries
    // exhaust the Wi-Fi TX queue. Retry the identical encrypted datagram here,
    // before returning to DTLS, so its pending record need not be abandoned.
    // Bound the wait; the peer loop must still service incoming traffic.
    while (result == -200 && esp_timer_get_time() - started < 250000) {
        vTaskDelay(pdMS_TO_TICKS(10));
        result = __real_udp_socket_sendto(socket, address, buffer, length);
    }
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
