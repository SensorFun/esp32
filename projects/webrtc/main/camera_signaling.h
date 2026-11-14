#pragma once
#include "esp_peer_signaling.h"

struct CameraSignalingConfig {
    const char *room;
    const char *secret;
    const char *stun;
};
const esp_peer_signaling_impl_t *camera_signaling_impl();
