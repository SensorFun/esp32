# Shared Wi-Fi provisioning

Native ESP-IDF component for ESP32/ESP32-S3 boards with built-in Wi-Fi. All setup
HTML, HTTP endpoints, scanning, reconnect logic, saved settings, and reset handling
live here. ESP32-P4 needs an external Wi-Fi driver and is not supported by this
component as configured.

Add `projects/common` to a project's `EXTRA_COMPONENT_DIRS` before including
ESP-IDF's `project.cmake`. Add `provisioning` to the application's `REQUIRES`, then
call once at startup:

```cpp
#include "provisioning.h"
const provisioning_config_t config = {
    .reset_gpio = WIFI_SETUP_BUTTON_GPIO_NUM,
    .reset_hold_ms = 5000,
    .serial_reset = true,
};
ESP_ERROR_CHECK(provisioning_start(&config));
```

The component owns the default Wi-Fi interfaces and Wi-Fi event handlers. It
initializes NVS and the default event loop; it does not erase NVS on initialization
errors. Call it before other application code starts Wi-Fi.

## Setup

1. With no saved settings, or after 20 seconds of failed connection attempts, the
   board starts an open `ESP32-Setup-…` network. Join it without a password;
   serial access is not needed.
2. Join that access point and open `http://192.168.4.1` in a browser. This is a
   direct setup page; it does not rely on a captive-portal popup or internet.
3. Refresh to scan visible SSIDs and choose one from the dropdown. Choose
   **Other network…** to enter a hidden network manually. Enter its password, or leave empty for open Wi-Fi.
4. Submit to connect. Only a successful connection with an assigned IP address
   commits the new settings. Invalid credentials leave the old settings intact,
   keep setup available, and allow another attempt. The browser polls status;
   the setup access point closes five seconds after a successful save.

Networks must be supported by the board's radio. The page accepts personal Wi-Fi
and open networks, not enterprise authentication. SSIDs are limited to 32 bytes;
passwords are empty, 8–63 bytes, or a 64-digit hexadecimal PSK.

Credentials are stored in the `wifi_setup` NVS namespace as one committed blob
and survive power loss. Saved passwords are never returned over HTTP; the
component never logs submitted credentials. This basic firmware uses ordinary
NVS, without encryption at rest. The temporary setup AP is open, so provisioning
traffic is not encrypted over the air. Only that AP interface serves setup; it
closes after a successful connection.

## Reset and recovery

- Hold the configured active-low setup/BOOT button for five seconds while the
  app is running, then release it. The board erases its provisioning credentials
  and restarts into setup. Waiting for release avoids rebooting GPIO0 boards
  into the ROM download loader.
- Type `wifi-reset` followed by Enter on the serial console when `serial_reset`
  is enabled. This works on the AI Thinker ESP32-CAM, which has no safe dedicated
  setup button: its GPIO0 is the camera clock, so its header specifies `-1`.
- Applications can call `provisioning_reset_credentials()` for another physical
  control or application-specific reset flow. Only this component's saved
  credentials are erased; unrelated NVS data is preserved.

If a saved network disappears or authentication fails, the board falls back to
setup after 20 seconds. Without a safe physical button, the serial reset command
can still clear settings while a saved network remains reachable.

## HTTP interface

All responses disable caching. Setup is available only through the access point's
IP and host name; state-changing requests from other origins are rejected.

| Route | Behavior |
| --- | --- |
| `GET /` | Embedded setup page with no external assets |
| `GET /api/scan` | Scan and return unique, nonempty SSIDs; hidden names are entered manually |
| `GET /api/status` | Connection state without credentials |
| `POST /api/connect` | URL-encoded `ssid` and `password`; returns 202 while connecting |

Do not put real network names or passwords in source, examples, tests, or configs.
