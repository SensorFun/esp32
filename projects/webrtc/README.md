# Local WebRTC camera

Uses the `esp_peer` component from Espressif's
[esp-webrtc-solution](https://github.com/espressif/esp-webrtc-solution), pinned in
`main/idf_component.yml`. JPEG frames and camera commands use a browser WebRTC
SCTP/DTLS data channel. This is JPEG over WebRTC, rather than an H.264 video track.
The device hosts its own local HTTPS SDP exchange; no external signaling/STUN/TURN
service or Internet connection is required. Browser and board must have local
IP connectivity (guest network/client isolation can prevent this). This local
mode requires direct IPv4 HTTPS access to the device, without a reverse proxy.
Every viewer, local or remote, enters the camera password.
Browser mDNS host candidates are mapped to the HTTPS socket's
source address, preserving their UDP ports, ICE credentials, and DTLS
fingerprint. Normal browser privacy settings can remain enabled.

## Build and flash

From the workspace root:

```sh
python tools/build.py project=webrtc device=esp32_cam
python tools/flash.py project=webrtc device=esp32_cam
python tools/build.py project=webrtc device=esp32s3_wroom
python tools/flash.py project=webrtc device=esp32s3_wroom
```

Builds run in the existing ESP-IDF 6.1 Docker environment. Flashing uses the
host virtual environment. The project has independent build directories and
preserves the NVS provisioning partition when flashing. The application fits
within 4 MB flash on both boards. S3 uses octal PSRAM; ESP32-CAM uses quad PSRAM.

## Provision and connect

In the browser flasher, select the USB port and flash the WebRTC firmware.
When the board restarts, the **Provision WebRTC camera** popup opens. Its setup
page talks to the board over USB, so the computer can stay on its usual Wi-Fi.
Choose the network and enter the camera password; the page then shows the
local viewer link. To move an
existing board to another network, select WebRTC camera and the board's port,
then choose **Provision** next to the flash button. The popup first checks
that the board runs the WebRTC firmware. A board that already has a camera
password keeps it, and the page does not ask for it again. If the board cannot
join the new network, it reconnects to the saved one. On boards that need
manual bootloader mode, release BOOT and reset before using USB setup.

Wi-Fi is provisioned only over USB. The board never opens a setup access
point, so nobody in radio range can move it to another network. Without saved
Wi-Fi it waits for USB provisioning. If its saved network becomes unavailable
(router restart, out of range), it keeps rejoining that network, pausing
15 seconds between 20-second attempts; USB setup can scan and switch networks
during the pauses.

1. Open the USB console at 115200 baud. Without saved Wi-Fi credentials, the
   board prints `No saved Wi-Fi; waiting for USB provisioning` and its HTTPS
   certificate SHA256 fingerprint.
2. Provision with the browser flasher's **Provision** popup, or from the
   command line:

   ```sh
   python tools/provision.py --port /dev/ttyUSB0 --camera-password
   ```

   It prompts for the network, Wi-Fi password, and camera password without storing them on the host.
   The console command `wifi-provision`, followed by SSID, Wi-Fi password, and camera password
   lines, submits through the same provisioning queue. USB is a physical trusted
   interface; its serial traffic is not cryptographically encrypted.
   Choose a separate camera password (12–96 UTF-8 bytes) for local and
   remote viewers.
3. After the station connects, the console prints
   `Local camera ready: https://<device-ip>/`. Join the same LAN, open that URL,
   enter the camera password and click **Connect**. Compare the certificate's
   SHA256 fingerprint with the USB console before accepting the device’s
   self-signed HTTPS certificate. Plain HTTP is not served: the password proof
   uses the browser's Web Crypto API, which needs HTTPS.
4. Use the displayed controls while watching the live camera. **Disconnect**
   releases the single peer. An abandoned session expires after 30 seconds
   without browser data-channel keepalives.

Certificates change after a reboot. Saved Wi-Fi settings
survive ordinary reboots. The existing `wifi-reset` and physical setup reset
clear saved Wi-Fi/camera settings and return to provisioning. Only the previous
public signaling membership ID is retained so the camera can release its own
stale room slot after reprovisioning.

## Camera controls

OV devices use `config/frame_grabber/ov.yaml`; the S3-WROOM device uses
`config/frame_grabber/gc.yaml` for GC0308. Each device selects
`config/lens/general.yaml` (currently without overrides). Sensor PID detection
selects the defaults and capture path at runtime. GC0308 captures YUV422 and
encodes JPEG in software (esp_new_jpeg, quality 60, with frame2jpg as fallback), with a
maximum resolution of VGA. An encoder task on CPU0 keeps one JPEG ready while the worker
sends the previous one, and two raw buffers let the sensor capture meanwhile. Unsupported sensor
controls are disabled, and OV light timing is omitted from its settings panel.
CMake generates initial values from both YAML files and rejects unknown controls
and invalid ranges. `gc.yaml` rejects controls absent from the GC0308 driver.
GC gain defaults to 10 (hardware global gain 0x15, near the driver default
0x14); the OV value of 0 would write zero global gain on this sensor.
All numeric settings in the current YAML profiles have sensor-driver mappings.
The viewer also exposes the other common image/exposure/white-balance controls.
OV2640 sharpness and denoise are disabled because its driver does not implement
them. Settings are validated on the device and acknowledged with the sensor
status; failures return errors. Both viewers use the same settings component at
`projects/common/camera_ui/settings.js`. Edits take effect immediately without
writing flash. **Save** validates and stores the complete configuration in the
board's NVS. Save and Reset buttons appear above and below the controls. Resolution, light
timing, and supported numeric controls are restored after reboot. **Reset to
Default** applies the firmware's YAML/driver defaults to the running camera.
Select **Save** to store those defaults in NVS. A restart before saving loads the
last saved settings. Invalid stored configurations fall back to defaults.

`timing_profile` offers **High light**, **Mid light**, and **Low light**
on both cameras. High restores the driver's resolution-specific native timing;
Mid uses a 2x line period. Low uses 3x for OV3660 (within its 13-bit HTS
limit) and 4x for OV2640. OV3660 changes HTS (`0x380c/0x380d`),
while OV2640 changes the sensor clock divider (sensor-bank `CLKRC`). Timing
registers are read back before acknowledging a change, and failures restore the
previous setting. Longer line periods allow longer exposure but reduce the
sensor's maximum frame rate.

The profile names match the server. The OV5647/P4's absolute HTS/VTS values are
sensor-specific. The shared OV profile defaults to **Mid light** (`mid_light`).

The stream is hardware JPEG, initially 320x240 unless saved settings select
another resolution. Streaming has no fixed FPS cap. Frames are split into
2048-byte chunks with a 16-byte little-endian
header (`JPEG` magic, frame sequence, total size, byte offset). The viewer
bounds-checks and reassembles frames. Backpressure retries are bounded and
incomplete frames are discarded. Commands are bounded JSON messages on the
same encrypted channel; no camera-control HTTP endpoints exist.
Frame capture waits at most 100 ms per poll so the worker continues processing
WebRTC traffic between missing frames. Eight seconds without a frame triggers
camera recovery while retaining the current settings.

The pinned `esp_peer` transport uses a 500 ms receive wait during ICE/DTLS/SCTP
setup and switches to 10 ms after the data channel opens. `peer_receive.c`
implements that transition, including resetting it for reconnects. This allows
STUN replies time to arrive while preserving short idle polls during streaming.
Peer liveness uses encrypted data-channel pings and the application's 30-second
inactivity deadline. The library's redundant periodic STUN keepalive checker is
disabled; initial ICE/STUN negotiation remains enabled.
Reliable data-channel packets are retained for 60 seconds, beyond the inactivity
deadline, to avoid discarding ordered messages during temporary backpressure.
`peer_send.c` gives the pinned UDP transport's explicit queue-full result a
250 ms retry budget (plus the final transport call), preserving the same
encrypted datagram. Other results pass through unchanged. Persistent failures
report packet size and available internal memory without recording payloads.

Setting commands run after the WebRTC receive callback returns and after releasing
any held camera frame. The command queue is bounded to 16 entries. Temporary
send-buffer exhaustion (`WOULD_BLOCK` or `NO_MEM`) retains the pending reply for
retry instead of disconnecting the viewer. Telemetry yields to queued replies.
Video and file sends use 2 KiB chunks and a 16 KiB SCTP send cache. Bulk payloads
are paced to 256 KiB/s; video may send up to three chunks of unused pacing credit
in one worker pass, and file downloads one chunk per pass. Settings and heartbeat
processing bypass that limit. The camera
disables Wi-Fi modem sleep to prevent a sleeping radio from delaying the live
stream and filling its TX queue. Video admission pauses below 12 KiB of free internal RAM or a
2 KiB largest free block, leaving the peer loop active to drain acknowledgements.
The PSRAM allocation threshold is 512 bytes to keep packet payloads out of scarce
internal RAM. Wi-Fi and lwIP buffers stay in internal RAM: copying them from
PSRAM slowed the ESP32's transmit queue enough to drop viewers. Camera JPEGs for streaming and recording stay in PSRAM. Motion
analysis keeps its two 8-bit frames (at most 160x120 each) in internal RAM; the ROM JPEG
decoder feeds blocks straight into the average, so no full decoded image is held.
On the ESP32 the decoder's 19 KiB of row sums and workspace go to PSRAM, and the
motion and recorder task stacks are in PSRAM (neither task writes flash). The peer
worker runs in the main task, sharing one stack. The worker, the camera task and
the camera interrupt run on CPU1, leaving CPU0 to Wi-Fi. Motion analysis may run
on either CPU at low priority, so it uses time Wi-Fi leaves idle while streaming
keeps CPU1 busy; it then analyses fewer frames than its configured rate.

On the ESP32-CAM the camera clock and the sensor's pixel bus run beside the PCB
antenna. At the default drive strengths they cut Wi-Fi throughput by more than ten
times, even with a strong signal and no capture running. The firmware sets the clock
pin and the OV2640 outputs to their weakest drive, which restores full Wi-Fi speed.

Optical-flow and grayscale loops cooperate with FreeRTOS between bounded work
blocks so the CPU 1 idle task can run; increasing the analysis rate cannot create
an uninterrupted analysis backlog that trips its watchdog. A requested analysis
rate is an upper limit, not a guarantee when the CPU is saturated.

Hardware regression (restores starting camera/motion settings):

```sh
.venv/bin/python tools/test_settings_stability.py --port /dev/ttyUSB1 --browser /path/to/chromium --rounds 2
```

These local tools prompt for the camera password, or read one stdin line with
`--password-stdin`. It changes every supported resolution, all timing profiles, image controls, and
both Camera/Motion Save paths under maximum configured analysis load, checking
frame progress, the same live channel, and USB watchdog/reset diagnostics.

Long-running local connection regression (does not request a board reset or change settings):

```sh
.venv/bin/python tools/test_stream_stability.py --url https://<camera-address>/ --port /dev/ttyUSB1 --browser /path/to/chromium --seconds 180 --reconnects 2
```

It checks continuous JPEG and settings replies, HTTPS responsiveness during
streaming, reconnects, and serial reset/watchdog diagnostics. Serial adapters may
pulse reset when opened, so the reset baseline starts after the stream is ready.

## Motion detection and SD recording

Both the board's local page and the website's remote WebRTC page show the same
**Motion detection** and **Recordings** panels (`projects/common/camera_ui/motion.js`),
driven by the same data-channel commands. The firmware describes every setting
(range, unit, group, help) in its `motion` reply, so both pages render the same form.
Changes apply on the camera right away, like camera settings; select Save to store
them in NVS so they survive a reboot.

- **Detection** runs on the board even with no viewer connected, and while Wi-Fi
  is down. Every analysed frame is decoded at 1/8 scale, the decoder's fastest mode:
  it keeps only each 8x8 block's average and skips the inverse transform. The grayscale
  analysis image is therefore 1/8 of the camera resolution (40 x 30 at 320 x 240,
  80 x 60 at 640 x 480, 160 x 90 at 1280 x 720); larger results are averaged to fit
  160 x 120 (1600 x 1200 gives 160 x 120). The motion status shows the current size.
  Single-level optical flow then runs over a grid of cells of about
  **Cell size** pixels that tiles the whole image (`main/optical_flow.cpp`), with global
  brightness changes removed. Speeds, cell size and smoothing are in analysis-image
  pixels, so a resolution change also changes how much of the scene they cover.
  A cell moves when its speed reaches **Minimum speed** (analysis pixels per second;
  the detector converts it using the measured time between analysed frames, so
  changing **Analysis rate** does not change sensitivity). Motion is reported when
  **Moving area** percent of cells move. Flat cells below **Minimum texture** are
  ignored. **Method** lists the available detectors; optical flow is the first.
- **Show motion in preview** draws the moving cells, MOTION, and REC over the live picture.
- **Recording settings** are in the **SD card recording** tab, above the recordings;
  their Save and Reset to Default affect only those fields, as the Motion detection
  tab's do for detection. Recording needs motion detection on.
  Recording starts after motion lasts **Motion duration** seconds. It includes
  **Pre-record** seconds from before that, continues **Keep recording** seconds after
  motion stops, and splits at **Maximum clip length**. Clips use the selected camera
  resolution, at up to **Recording rate** frames per second. A resolution change starts
  a new clip. With **Replace oldest clips when full**, the oldest clips are deleted
  to keep 2% of the card (at least 64 MB) free; otherwise recording pauses.
- **Recordings** lists clips newest first with local start time, length, resolution,
  and size. Play, Download (`.avi`), and Delete work over the same encrypted channel;
  the live preview pauses while a clip downloads. Clips are Motion-JPEG AVI files that
  play in VLC and most desktop players. The browser player also plays clips cut short
  by a power loss; data is synced to the card every 3 seconds.

The microSD slot uses SDMMC one-bit mode, with pins from the board header's
`SD_MMC_*` macros: GPIO 14/15/2 on the AI Thinker ESP32-CAM and GPIO 39/38/40 on the
S3 boards. Boards without these macros report no SD slot. Use a FAT32 card; the
firmware never formats a card. Clips are stored as `REC/YYYYMMDD/HHMMSS.AVI` in UTC
(8.3 names). The clock comes from SNTP (`CONFIG_CAMERA_SNTP_SERVER`, default
`pool.ntp.org`), or from the first connected viewer until SNTP answers.

While you are watching, recordings use the frames being streamed, so they record at
no more than the live frame rate. Motion and SD activity add CPU and memory load.
On the original ESP32, large resolutions record fewer frames per second.

## Security scope

Wi-Fi provisioning and camera password changes need physical USB access; the
board opens no setup access point. Local HTTPS delivers the DTLS fingerprint,
and the browser WebRTC stack negotiates DTLS encryption for images and controls.
Local viewers prove the camera password exactly as remote viewers do (see
[Remote authentication](#remote-authentication)); the camera sends no frames,
settings or recordings and accepts no commands before that. A board without a
camera password refuses local sessions. The API checks Host and Origin and
does not enable CORS. The viewer loads no remote assets. Plain HTTP is not
served. Public website/signaling access uses HTTPS/WSS. The device's HTTPS
certificate is self-signed and changes after each reboot, so compare its
fingerprint with the USB console: a look-alike page served by another device
on the LAN could otherwise capture the password you type.

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
HTTPS certificate, checks that plain HTTP is closed and the origin rules, signs in with the camera password in a real browser,
decodes camera JPEG, exercises sensor controls and invalid values, inspects
DTLS connection statistics, and reconnects. It writes no credentials or camera
images. Keep serial monitors closed while flashing or running either USB tool.

The full hardware E2E suite (`python -m pytest tests/e2e -v -s`) also runs
`tools/test_https_reconnect.py` and `tools/test_camera_settings.py` after its
public-signaling browser checks, passing the run's camera password on stdin. The HTTPS test checks HTML, the shared settings
module, parallel requests, settings and video after repeated hardware resets.
On the first boot it also checks the camera password: plain HTTP is closed, a
session that skips authentication gets only authentication errors (no frames,
settings or recordings), and a wrong password is rejected.
The viewer allows four HTTPS sockets so browser parallel connections do not
evict a controls-module download, and enables Connect only after controls load.
Three randomized rounds change at least five supported image controls, resolution,
and light timing. Each round saves through the GUI, restarts the board over USB,
opens a new viewer, and compares the complete reported configuration. It also
checks unsaved changes and Reset to Default across restarts, then restores the
original settings. The printed seed can be replayed with `--seed`; use `--rounds`
to increase coverage. The persistence tool needs `--url` and `--port`; snapshots
are optional and contain settings only.

## Private signaling

Cameras use `wss://signalserver.sensorfun.net/camera` and the website uses
`wss://signalserver.sensorfun.net/viewer`. These are fixed service endpoints.
Provisioning stores Wi-Fi credentials and the camera password. Firmware ignores
legacy endpoint values in NVS. The server API secret is an internal service
credential embedded in authorized firmware; keep private builds and sdkconfig
out of published artifacts.

The signaling server forwards signaling only; camera images and controls remain on the direct
encrypted WebRTC connection. The viewer asks only for the camera MAC and camera
password. The signaling API secret stays on the server and in camera firmware.

## Remote authentication

Remote viewing requires the MAC and the camera password chosen during provisioning;
local viewing requires the password (the board's page reads the MAC from the
challenge). The ESP32 sends no JPEG frames or settings and rejects commands until password
authentication succeeds. It stores a salted PBKDF2-HMAC-SHA256 verifier (210,000
iterations), never the password. The data-channel challenge/response uses the
[SCRAM key/proof construction](https://www.rfc-editor.org/rfc/rfc5802.html#section-3)
with SHA-256. This is a project-specific JSON protocol, not an RFC SCRAM wire
implementation. Its transcript binds a random challenge, camera room, salt,
iteration count, and both WebRTC SDP certificate fingerprints. The browser also
checks the camera's server signature. Replayed proofs from other connections
fail. The password never leaves the browser and is never compared there: the
page turns it into a per-connection proof, and the board decides whether that
proof matches its stored verifier. Both viewers clear the typed password as
soon as Connect starts and keep only a non-extractable Web Crypto key derived
from it, so Connect can reconnect; a rejected password discards that key. Five failed proofs cause a 30-second cooldown; pending authentication
expires after 15 seconds. Changing/clearing the password invalidates current sessions.
Use a strong unique camera password; password-derived proofs allow offline
password guessing by a malicious peer that records an authentication exchange.

For a board already provisioned with Wi-Fi, set its camera password without
resetting Wi-Fi:

```sh
.venv/bin/python tools/camera_password.py --port /dev/ttyUSB0
```

The tool prompts securely and confirms the password. `--clear` disables local
and remote viewing until a password is set again. These commands require physical USB access.
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
