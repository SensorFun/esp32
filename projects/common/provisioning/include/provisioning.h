#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start once after common setup. Owns Wi-Fi and the default network interfaces.
// Uses the default NVS partition. Without saved credentials it waits for USB
// provisioning; it never opens a setup access point.
esp_err_t provisioning_start(void);
bool provisioning_is_ready(void);
// USB console provisioning submits through the shared validation/queue.
esp_err_t provisioning_submit_credentials(const char *ssid, const char *password);
esp_err_t provisioning_submit_credentials_with_camera(const char *ssid, const char *password, const char *camera_password);
// USB setup commands (status, scan, wifi, password). Replies are printed as
// SENSORFUN_USB <json> lines for the local serial client.
void provisioning_usb_command(const char *command);

#ifdef __cplusplus
}
#endif
