#pragma once
#include <stdbool.h>

enum { CAMERA_ICE_RECEIVE_MS = 500, CAMERA_STREAM_RECEIVE_MS = 10 };

#ifdef __cplusplus
extern "C" {
#endif
void camera_peer_transport_ready(bool ready);
#ifdef __cplusplus
}
#endif
