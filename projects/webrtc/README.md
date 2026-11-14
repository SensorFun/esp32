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
   board starts an open `CAM_<MAC>` setup network and prints its HTTPS
   certificate SHA256 fingerprint. Joining the setup Wi-Fi needs no password.
   The separate camera password is chosen on the provisioning page.
2. Join that network and open `https://192.168.4.1`. Compare the certificate's
   SHA256 fingerprint with the physical USB console before accepting the
   self-signed certificate. Select a network, enter its Wi-Fi password, and
   choose a separate camera password (12–96 UTF-8 bytes) for remote viewers.
   Alternatively, provision through the physical USB console:

   ```sh
   python tools/provision.py --port /dev/ttyUSB0 --camera-password
   ```

   It prompts for the network, Wi-Fi password, and camera password without storing them on the host.
   The console command `wifi-provision`, followed by SSID, Wi-Fi password, and camera password
   lines, submits through the same provisioning queue. USB is a physical trusted
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
clear saved Wi-Fi/camera settings and return to provisioning. Only the previous
public signaling membership ID is retained so the camera can release its own
stale room slot after reprovisioning.

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

The setup AP is open and requires no Wi-Fi password. HTTPS protects
provisioning credentials and SDP. Local
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

## Private Janus signaling

The [Janus and Cloudflare Tunnel stack](../../tools/signal_server/README.md) runs on your
computer and lets the GitHub Pages viewer connect through your domain. Configure
**Camera remote signaling** in menuconfig with the device API secret and STUN
service, then rebuild and flash. Choose **signalserver.sensorfun.net** (default)
or **My own signaling server…** on the Wi-Fi provisioning page. Custom server
URLs must use `wss://` and end in `/camera`; this choice is saved in NVS.
The selected server must accept the device API secret. Store credentials only in the ignored
build sdkconfig, never in tracked defaults. The signaling secret is embedded in
the resulting firmware, so keep binaries private.

Janus forwards signaling only; camera images and controls remain on the direct
encrypted WebRTC connection. The viewer asks only for the camera MAC and camera
password. The site owner configures its `/viewer` endpoint at deployment; the
Janus API secret stays on the server and in camera firmware. With no saved
selection, the firmware uses `wss://signalserver.sensorfun.net/camera`.

## Legacy Espressif test tooling

The following describes the older Espressif test tools retained in the repository;
the current firmware uses the Janus server chosen during provisioning instead.
The stable room is exactly the station MAC as 12 uppercase hex digits, without separators. Browser and board derive the same room from the MAC. The `CAM_<MAC>` setup
network name contains this station MAC; keep it for remote viewing. No USB discovery
is required in normal operation. The browser creates the offer, and the camera answers using
STUN information supplied by the service. TURN media relaying is disabled. The original local HTTPS viewer is
still available; only one viewer can use a board at a time.

See [the end-to-end guide](../../site/scripts/README.md). The test serves a static
React production build locally and uses a small signaling bridge because the
hosted service's HTTP endpoints do not enable browser cross-origin access.
No JPEG or camera-control messages pass through that bridge. The board's
signaling HTTPS/WSS connections verify the service certificate through the IDF
certificate bundle. DTLS continues to encrypt the WebRTC data channel.

Remote viewing requires the MAC and the camera password chosen during provisioning.
The ESP32 sends no JPEG frames or settings and rejects commands until password
authentication succeeds. It stores a salted PBKDF2-HMAC-SHA256 verifier (210,000
iterations), never the password. The data-channel challenge/response uses the
[SCRAM key/proof construction](https://www.rfc-editor.org/rfc/rfc5802.html#section-3)
with SHA-256. This is a project-specific JSON protocol, not an RFC SCRAM wire
implementation. Its transcript binds a random challenge, camera room, salt,
iteration count, and both WebRTC SDP certificate fingerprints. The browser also
checks the camera's server signature. Replayed proofs from other connections
fail. Five failed proofs cause a 30-second cooldown; pending authentication
expires after 15 seconds. Changing/clearing the password invalidates remote sessions.
Use a strong unique camera password; password-derived proofs allow offline
password guessing by a malicious peer that records an authentication exchange.

For a board already provisioned with Wi-Fi, set its camera password without
resetting Wi-Fi:

```sh
.venv/bin/python tools/camera_password.py --port /dev/ttyUSB0
```

The tool prompts securely and confirms the password. `--clear` disables remote
access until a password is set again. These commands require physical USB access.
Do not run a serial monitor or the test launcher at the same time.
Initial camera password derivation can take 1–2 minutes on ESP32-CAM; subsequent logins use the stored verifier.

Espressif's public room remains a public rendezvous: the password protects camera
video/control access, not room membership or availability. Someone knowing the
MAC can occupy a room slot or disrupt signaling. A private authenticated signaling
service is needed to control membership itself. Public deployment also needs a
public HTTPS/WSS bridge endpoint; the local bridge listens only on loopback and
should sit behind a TLS reverse proxy with request/connection limits. GitHub Pages
cannot host that backend process. Configure its exact frontend origin. No
password is sent through the signaling bridge or saved in browser storage.
Flash/NVS encryption remains disabled: a physically extracted verifier is not
protected by this change.

Remote connections are direct-only. Restrictive NATs may prevent connections;
the firmware does not silently enable TURN as a fallback.
