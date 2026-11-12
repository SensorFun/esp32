# Local WebRTC camera

Uses the `esp_peer` component from Espressif's
[esp-webrtc-solution](https://github.com/espressif/esp-webrtc-solution), pinned in
`main/idf_component.yml`. JPEG frames and camera commands use a browser WebRTC
SCTP/DTLS data channel. This is JPEG over WebRTC, rather than an H.264 video track.
The device hosts its own HTTPS SDP exchange; no external signaling/STUN/TURN
service or Internet connection is required. Browser and board must have local
IP connectivity (guest network/client isolation can prevent this). This local
mode requires direct IPv4 HTTPS access to the device, without a reverse proxy.
Browser mDNS host candidates are mapped to the HTTPS socket's
source address, preserving their UDP ports, ICE credentials, and DTLS
fingerprint. Normal browser privacy settings can remain enabled.

## Build and flash

From the workspace root:

```sh
python tools/build.py project=webrtc device=esp32_cam_ov2640_62
python tools/flash.py project=webrtc device=esp32_cam_ov2640_62
python tools/build.py project=webrtc device=esp32s3_wroom_ov3660_62
python tools/flash.py project=webrtc device=esp32s3_wroom_ov3660_62
```

Builds run in the existing ESP-IDF 6.1 Docker environment. Flashing uses the
host virtual environment. The project has independent build directories and
preserves the NVS provisioning partition when flashing. The application fits
within 4 MB flash on both boards. S3 uses octal PSRAM; ESP32-CAM uses quad PSRAM.

## Provision and connect

1. Open the USB console at 115200 baud. Without saved Wi-Fi credentials, the
   board prints a `CAM_<MAC>` setup network, a random WPA2 password, an HTTPS
   certificate SHA256 fingerprint. Secrets are generated
   on-device for each boot; none are compiled into firmware.
2. Join that network and open `https://192.168.4.1`. Compare the certificate's
   SHA256 fingerprint with the physical USB console before accepting the
   self-signed certificate. Select a network and enter its password.
   Alternatively, provision through the physical USB console:

   ```sh
   python tools/provision.py --port /dev/ttyUSB0
   ```

   It prompts for the network and password without storing either on the host.
   The console command `wifi-provision`, followed by an SSID line and password
   line, submits through the same provisioning queue. USB is a physical trusted
   interface; its serial traffic is not cryptographically encrypted.
3. After the station connects and the setup AP closes, the console prints
   `Local camera ready: https://<device-ip>/`. Join the same LAN, open that URL,
   click **Connect**. No pairing, access key, or login is required on the LAN.
   Your browser may show a certificate warning for the device’s self-signed HTTPS certificate.
4. Use the displayed controls while watching the live camera. **Disconnect**
   releases the single peer. An abandoned session expires after 30 seconds
   without browser data-channel keepalives.

Certificates change after a reboot. Saved Wi-Fi settings
survive ordinary reboots. The existing `wifi-reset` and physical setup reset
clear NVS and return to provisioning.

## Camera controls

The physical sensor PID chooses `config/frame_grabber/ov2640.yaml` or
`ov3660.yaml`, even when a board supports both sensors. CMake generates initial
values directly from those files and rejects unknown fields or invalid ranges.
All numeric settings in the current YAML profiles have sensor-driver mappings.
The viewer also exposes the other common image/exposure/white-balance controls.
OV2640 sharpness and denoise are disabled because its driver does not implement
them. Settings are validated on the device and acknowledged with the sensor
status; failures return errors. Changes are volatile and YAML defaults are
reapplied on reboot.

`timing_profile` offers **High light**, **Mid light (balanced)**, and **Low light**
on both cameras. High restores the driver's resolution-specific native timing;
Mid uses a 2x line period. Low uses 3x for OV3660 (within its 13-bit HTS
limit) and 4x for OV2640. OV3660 changes HTS (`0x380c/0x380d`),
while OV2640 changes the sensor clock divider (sensor-bank `CLKRC`). Timing
registers are read back before acknowledging a change, and failures restore the
previous setting. Longer line periods allow longer exposure but reduce the
sensor's maximum frame rate; the stream still has its separate 5 fps cap.

The profile names match the server. The OV5647/P4's absolute HTS/VTS values are
sensor-specific and are not copied to these sensors. Both sensor YAML files
can select `high_light`, `balanced`, or `low_light` as a startup default.

The initial stream is hardware JPEG, 320x240, capped at 5 fps to accommodate
ESP32-CAM. Frames are split into 4096-byte chunks with a 16-byte little-endian
header (`JPEG` magic, frame sequence, total size, byte offset). The viewer
bounds-checks and reassembles frames. Backpressure retries are bounded and
incomplete frames are discarded. Commands are bounded JSON messages on the
same encrypted channel; no camera-control HTTP endpoints exist.

## Security scope

HTTPS protects provisioning and SDP; WPA2 protects the setup AP. Local
HTTPS delivers the DTLS fingerprint, and the browser WebRTC stack
negotiates DTLS encryption for images and controls. The API checks Host
and Origin and does not enable CORS. Any device with LAN access can view and
control the camera; no pairing token or application login is used. The viewer
loads no remote assets. Plain HTTP is not started by this project.

This implements encryption in transit, not encryption of flash contents. Saved
Wi-Fi credentials remain in the existing NVS partition. Secure boot, flash/NVS
encryption, and irreversible eFuse changes are not enabled. Anyone with physical
USB access can provision/reset the device.

## Live verification

Install Playwright in the host environment and its Chromium browser. After
provisioning, run:

```sh
python tools/test_local_webrtc.py --port /dev/ttyUSB0 --browser /path/to/chromium
```

The test resets the board, reads its ephemeral identity over USB, compares the
HTTPS certificate, checks access without credentials and origin rules, connects a real browser,
decodes camera JPEG, exercises sensor controls and invalid values, inspects
DTLS connection statistics, and reconnects. It writes no credentials or camera
images. Keep serial monitors closed while flashing or running either USB tool.
