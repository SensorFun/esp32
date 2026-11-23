#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// Count of Wi-Fi TX queue-full results from the UDP transport (see peer_send.c).
uint32_t camera_transport_queue_full(void);
#ifdef __cplusplus
}
#endif
