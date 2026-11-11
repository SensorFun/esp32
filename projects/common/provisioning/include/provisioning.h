#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start once after common setup. Owns Wi-Fi and the default network interfaces.
// Uses the default NVS partition; opens setup if no working credentials exist.
esp_err_t provisioning_start(void);

#ifdef __cplusplus
}
#endif
