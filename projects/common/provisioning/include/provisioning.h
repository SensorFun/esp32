#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start once after common setup. Owns Wi-Fi and the default network interfaces.
// Uses the default NVS partition; opens setup if no working credentials exist.
esp_err_t provisioning_start(void);
bool provisioning_is_ready(void);
// USB console and setup page submit through the same validation/queue.
esp_err_t provisioning_submit_credentials(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
