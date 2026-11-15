#include "freertos/FreeRTOS.h"
#include "cam_hal.h"

extern camera_fb_t *__real_cam_take(TickType_t timeout);

camera_fb_t *__wrap_cam_take(TickType_t timeout)
{
    // esp_camera_fb_get normally waits four seconds. It runs in the same
    // task as SCTP, so let the network loop run between short capture waits.
    const TickType_t limit = pdMS_TO_TICKS(100);
    return __real_cam_take(timeout < limit ? timeout : limit);
}
