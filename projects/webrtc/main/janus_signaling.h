#pragma once
#include "esp_peer_signaling.h"

struct JanusSignalingConfig {
    const char *room;
    const char *secret;
    const char *stun;
};
const esp_peer_signaling_impl_t *janus_signaling_impl();
