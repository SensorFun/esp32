#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int reset_gpio;             // Active-low button; -1 disables physical reset.
    uint32_t reset_hold_ms;      // Hold while running, then release to reboot.
    bool serial_reset;          // Accept "wifi-reset" followed by Enter on stdin.
} provisioning_config_t;

// Start once at application startup. Owns Wi-Fi, default netifs and its NVS namespace.
// Starts a setup AP when no credentials exist or a connection cannot be made.
esp_err_t provisioning_start(const provisioning_config_t *config);

// Erase only provisioning credentials and restart into setup mode asynchronously.
void provisioning_reset_credentials(void);

#ifdef __cplusplus
}
#endif
