#include "peer_receive.h"
#include "udp.h"
#include <stdatomic.h>

// One camera peer is active at a time. The pinned esp_peer implementation
// shares its UDP receive timeout between STUN gathering and the streaming loop.
static atomic_bool channel_ready = false;

void camera_peer_transport_ready(bool ready)
{
    atomic_store(&channel_ready, ready);
}

extern int __real_udp_socket_recvfrom_nowait(udp_socket_t *, esp_peer_addr_t *, uint8_t *, int, bool);

int __wrap_udp_socket_recvfrom_nowait(udp_socket_t *socket, esp_peer_addr_t *address,
                                    uint8_t *buffer, int length, bool nowait)
{
    if (!nowait) {
        udp_blocking_timeout(socket, atomic_load(&channel_ready)
                             ? CAMERA_STREAM_RECEIVE_MS : CAMERA_ICE_RECEIVE_MS);
    }
    return __real_udp_socket_recvfrom_nowait(socket, address, buffer, length, nowait);
}
