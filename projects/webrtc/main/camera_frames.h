#pragma once
#include "esp_camera.h"

// Streaming frames are always JPEG, including software-encoded raw sensors.
camera_fb_t *camera_frame_get();
void camera_frame_return(camera_fb_t *frame);
