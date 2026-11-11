# Shared Wi-Fi provisioning

Native ESP-IDF component for ESP32/ESP32-S3 boards with built-in Wi-Fi. All setup
HTML, HTTP endpoints, scanning, reconnect logic, saved settings, and reset handling
live here. ESP32-P4 needs an external Wi-Fi driver and is not supported by this
component as configured.

Projects normally use [common::Application](../application/README.md), which
initializes NVS and board reset handling, then calls `provisioning_start()` on
boards with integrated Wi-Fi. For lower-level use, add `provisioning` to a
component's `REQUIRES` and call `provisioning_start()` once. It owns the default
Wi-Fi interfaces/event handlers and initializes NVS/the default event loop. It
does not erase NVS on initialization errors.

## Setup

1. With no saved settings, or after 20 seconds of failed connection attempts, the
   board starts an open `CAM_<MAC>` network. Join it without a password;
   serial access is not needed. The suffix is the board's station Wi-Fi MAC
   in uppercase, colon-separated form; the SSID stays the same across reboots.
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

Reset inputs and the serial `wifi-reset` command belong to the shared
[application class](../application/README.md), not the Wi-Fi component.

- ESP32-CAM: connect IO12 to GND for two seconds, then remove the jumper.
- ESP32-S3: hold BOOT for two seconds while running, then release it.
- Applications can call `common.enter_setup()` directly.

These reset requests clear the entire default NVS partition on the next boot,
including saved Wi-Fi and other application settings. Ordinary power cycles keep
settings. If a saved network disappears or authentication fails, the Wi-Fi
component opens provisioning after 20 seconds without erasing saved settings.

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
