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

Wi-Fi is provisioned only over the physical USB console. The board never opens
a setup access point: an open one would let anyone in radio range move the
board to a network of their choosing.

1. Without saved settings the board logs `No saved Wi-Fi; waiting for USB
   provisioning` and waits.
2. Provision over USB with the browser flasher's **Provision** popup (it shows
   [index.html](index.html) and relays its requests as USB commands, see
   below), `tools/provision.py`, or the console command `wifi-provision`.
3. Refresh to scan visible SSIDs and choose one from the dropdown. Choose
   **Other network…** to enter a hidden network manually. Enter its password, or leave empty for open Wi-Fi.
4. For camera projects, set the camera password. Every viewer, local or
   remote, enters it.
5. Submit to connect. Only a successful connection with an assigned IP address
   commits the new settings. If a new network fails, the board rejoins its
   saved one; without a saved network it keeps waiting for USB provisioning.

Networks must be supported by the board's radio. The page accepts personal Wi-Fi
and open networks, not enterprise authentication. SSIDs are limited to 32 bytes;
passwords are empty, 8–63 bytes, or a 64-digit hexadecimal PSK.

Credentials are stored in the `wifi_setup` NVS namespace as one committed blob
and survive power loss. Saved passwords are never reported back; the component
never logs submitted credentials. This basic firmware uses ordinary NVS,
without encryption at rest. USB is a physical trusted interface; its serial
traffic is not encrypted.

## Reset and recovery

Reset inputs and the serial `wifi-reset` command belong to the shared
[application class](../application/README.md), not the Wi-Fi component.

- ESP32-CAM: connect IO12 to GND for two seconds, then remove the jumper.
- ESP32-S3: hold BOOT for two seconds while running, then release it.
- Applications can call `common.enter_setup()` directly.

These reset requests clear the entire default NVS partition on the next boot,
including saved Wi-Fi and other application settings. Ordinary power cycles keep
settings. If a saved network disappears or authentication fails, the Wi-Fi
component keeps rejoining it: each 20-second attempt is followed by a
15-second pause in which USB setup can scan and choose another network. Saved
settings are not erased.

## USB interface

With `CONFIG_PROVISIONING_SECURE=y`, the console accepts `sensorfan-usb <command>`
lines and answers with `SENSORFUN_USB <json>` lines:

| Command | Behavior |
| --- | --- |
| `status` | Firmware project, last setup result, and the local IP and remote viewer link once connected |
| `scan` | Visible SSIDs and whether a camera password is required; `busy` while joining a network |
| `wifi <form>` | URL-encoded `ssid` and `password`; camera projects also accept `camera_password`; replies `pending` |
| `password <form-encoded password>` | Set the camera password |

Do not put real network names or passwords in source, examples, tests, or configs.
