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
   releases the single peer, and closing the tab does too. A viewer whose
   connection is lost (network drop, sleeping phone, throttled background tab)
   keeps the single session for up to 5 minutes without any message; then the
   camera disconnects it.

Certificates change after a reboot. Saved Wi-Fi settings
survive ordinary reboots. The existing `wifi-reset` and physical setup reset
clear saved Wi-Fi/camera settings and return to provisioning. Only the previous
public signaling membership ID is retained so the camera can release its own
stale room slot after reprovisioning.

## Camera controls

Each sensor's profile is its frame grabber family's shared settings plus its own
file in `config/frame_grabber/sensors/` (`ov2640.yaml`, `ov3660.yaml`,
`gc0308.yaml`). `ov.yaml` lists the OV sensors and holds what they share in
`common`: the light timing, `clock_2x`, and 15 identical controls (JPEG quality,
mirroring, white balance, exposure switches, pixel corrections). A sensor file
holds only its differing controls (ranges, gain ceiling, OV3660 sharpness and
denoise) and overrides any shared entry it repeats. `gc.yaml` only lists its
sensor. Each device also selects
`config/lens/general.yaml` (currently without overrides). The firmware embeds
all three profiles and picks one by sensor PID at runtime, so the settings panel
always shows the installed sensor's controls and nothing else. GC0308 captures
YUV422 and encodes JPEG in software (esp_new_jpeg, quality 60, with frame2jpg as
fallback), with a maximum resolution of VGA. An encoder task on CPU0 keeps one
JPEG ready while the worker sends the previous one, and two raw buffers let the
sensor capture meanwhile. OV light timing is omitted from its settings panel.

A profile gives every control a default and its allowed values, taken from the
pinned esp32-camera driver and matching Espressif's CameraWebServer limits:
`switch: true` (shown as an On/Off switch), `options` (a list of values, with
labels such as Sunny/Cloudy or 2x-128x) or `range` (a number field, used only
for controls with many values: JPEG quality, AGC gain, AEC value and the
OV3660's raw gain ceiling). Several OV3660 ranges are wider than the OV2640's
(brightness/contrast -3 to 3, saturation -4 to 4, AE level -5 to 5, AGC gain
0-64, AEC value 0-1536); its gain ceiling is a raw register value with the
driver's reset value 248 as the default, not the OV2640's 2x-128x scale. It is
limited to 0-255 because the driver keeps it in an 8-bit status field. Each
number field has a 10-step slider beside it that moves between 11 evenly spaced
values from minimum to maximum; the number field still accepts any exact value.
CMake generates the tables from the YAML and rejects unknown controls,
malformed value sets and defaults outside them. GC gain defaults to 10
(hardware global gain 0x15, near the driver default 0x14); 0 would write zero
global gain on this sensor. The OV2640 driver implements neither sharpness nor
denoise, and the GC0308 driver no brightness or sensor JPEG quality, so their
profiles omit them. The camera reports each control's kind, range and labels;
it accepts only listed values. Settings are acknowledged with the sensor
status; failures return errors. The board's page, the website and the firmware
use one settings component, `projects/common/camera_ui/settings.js`: the
firmware embeds it for the local viewer and the website imports it. Edits take effect immediately without
writing flash. **Save** validates and stores the complete configuration in the
board's NVS. Save and Reset buttons appear above and below the controls. Resolution, light
timing, and all of the sensor's controls are restored after reboot. **Reset to
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

`clock_2x` (shared: `{enabled: true, max_resolution: 800x600}`) doubles the
sensor clock up to `max_resolution`, which raised streaming speed on the
ESP32-CAM; larger sizes keep the driver's clock. A sensor file can override
either key. The OV2640 sets its internal doubler (`CLKRC` bit 7), which Light
timing's divider bits leave intact. Above 800 x 600 the OV2640 switches to its
UXGA mode, where the doubled pixel clock outran capture and every JPEG lost its
end marker, so 1600 x 1200 never streamed with the doubler on. On the OV3660 it is experimental: the
driver's JPEG PLL pre-divider changes from /3 to /1.5 (`0x303D`), and the pixel
clock divider (`0x3824`) doubles first, so the ESP32 still receives a 10 MHz
pixel clock; other PLL settings are left alone. The driver resets these
registers on each resolution change, and the firmware reapplies them. With the
default Mid light, the doubled OV2640 clock divided by two equals the native
clock; High light runs it at twice the native rate. Settings replies report the
readback as `clock_2x`.

The stream is hardware JPEG, initially 320x240 unless saved settings select
another resolution. Streaming has no fixed FPS cap. The viewers' FPS is the
camera's: frames grabbed per second for live video, motion detection or
recording, sent in each `motion_status` as `capture_fps`. It stays meaningful
with streaming off, and reads 0 only when nothing uses the camera. Frames are split into
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
Peer liveness uses encrypted data-channel pings every 5 seconds and the
application's 5-minute inactivity deadline (recording transfers count as activity). The library's redundant periodic STUN keepalive checker is
disabled; initial ICE/STUN negotiation remains enabled.
Reliable data-channel packets are retained for 60 seconds (the library's
16-bit millisecond limit is 65.5 seconds), to avoid discarding ordered messages during temporary backpressure.
`peer_send.c` gives the pinned UDP transport's explicit queue-full result a
250 ms retry budget (plus the final transport call), preserving the same
encrypted datagram. Other results pass through unchanged. Persistent failures
report packet size and available internal memory without recording payloads.

Setting commands run after the WebRTC receive callback returns and after releasing
any held camera frame. The command queue is bounded to 16 entries. Temporary
send-buffer exhaustion (`WOULD_BLOCK` or `NO_MEM`) retains the pending reply for
retry instead of disconnecting the viewer. Telemetry yields to queued replies.
Video and file sends use 2 KiB chunks and a 64 KiB SCTP send cache (in PSRAM;
16 KiB limited video to one window per acknowledgement, about 190 KiB/s).
Bulk payloads (video and recording downloads) have no rate limit. Each worker
pass sends up to 12 chunks; a burst ends early when the SCTP send cache is full
(`WOULD_BLOCK`/`NO_MEM`) or the Wi-Fi TX queue fills, so the peer loop can
process acknowledgements before sending more. Video and downloads wait while a
control reply is pending, so settings and heartbeats are never starved. Every
5 seconds a viewer is connected, the log prints a `Transport:` line counting
full send caches, full TX queues, passes with a waiting reply, and the Wi-Fi
RSSI. On the ESP32-S3 the WebRTC library's send path (DTLS and SCTP) itself tops
out near 260 KiB/s, so the largest JPEGs are limited by it. The camera
disables Wi-Fi modem sleep to prevent a sleeping radio from delaying the live
stream and filling its TX queue. Video admission pauses below 12 KiB of free internal RAM or a
2 KiB largest free block, leaving the peer loop active to drain acknowledgements.
The PSRAM allocation threshold is 512 bytes to keep packet payloads out of scarce
internal RAM. Wi-Fi and lwIP buffers stay in internal RAM: copying them from
PSRAM slowed the ESP32's transmit queue enough to drop viewers. Camera JPEGs for live streaming and recording stay in PSRAM.
Native JPEG sensors capture 160 × 120 by default, into two PSRAM camera
buffers, whether or not motion detection is on. The selected resolution is used
only while live video streams or the SD card records (manually, or once motion
is confirmed); selecting 160 × 120 never restarts the camera. Each switch
restarts the driver, retrying once with sensor power cut. High-resolution JPEGs
are downscaled for motion analysis. The motion and recorder worker
stacks remain in PSRAM. Camera capture and the peer worker run on CPU1.

On the ESP32-CAM the camera clock and the sensor's pixel bus run beside the PCB
antenna. At the default drive strengths they cut Wi-Fi throughput by more than ten
times, even with a strong signal and no capture running. The firmware sets the clock
pin and the OV2640 outputs to their weakest drive, which restores full Wi-Fi speed.
The OV3660 driver sets 4x pad drive; the firmware lowers it to 1x as well, which
stopped Wi-Fi TX queue overflows on the Goouuu ESP32-S3 board.

Motion analysis has no rate setting: each finished frame admits the next, so it
runs as fast as the camera and the CPU allow (the motion status shows the
achieved rate). The motion task runs on CPU0 at priority 1: the peer worker,
camera task and camera interrupt own CPU1, and Wi-Fi and lwIP on CPU0 preempt
analysis. Optical-flow and grayscale loops yield between bounded work blocks;
the idle task, which feeds the watchdog, runs while the task waits for each frame.

Hardware regression (restores starting camera/motion settings):

```sh
.venv/bin/python tests/webrtc/manual/check_settings_stability.py --port /dev/ttyUSB1 --browser /path/to/chromium --rounds 2
```

These local tools prompt for the camera password, or read one stdin line with
`--password-stdin`. It changes every supported resolution, all timing profiles, image controls, and
both Camera/Motion Save paths under maximum configured analysis load, checking
frame progress, the same live channel, and USB watchdog/reset diagnostics.

Long-running local connection regression (does not request a board reset or change settings):

```sh
.venv/bin/python tests/webrtc/manual/check_stream_stability.py --url https://<camera-address>/ --port /dev/ttyUSB1 --browser /path/to/chromium --seconds 180 --reconnects 2
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

Defaults and allowed values for both tabs are in `config/motion/motion.yaml`, in
the same `switch`/`options`/`range` format as the sensor profiles (a `range` is
`[min, max, step]`). The build generates `motion_config.h` from it
(`main/generate_motion.py`); after editing it, build and then flash (`tools/flash.py`
flashes the last build without rebuilding). A missing or unknown
setting, or a default outside its allowed values, fails the build. Settings saved
on the camera take precedence: select Reset to Default, or flash with
`clear_nvs=true`, to use new defaults. A sensor restart (a capture size switch or
recovery) pauses analysis for 1 s while auto exposure settles, so the change in
brightness is not reported as motion.

- **Detection** runs on the board even with no viewer connected, and while Wi-Fi
  is down. Native JPEG cameras capture at 160 × 120 by default; the **Detect
  motion** checkbox only runs the algorithm. The selected resolution is used for
  live streaming and for recording once motion is confirmed and the SD card is
  ready. Motion analysis uses up to 160 × 120 luma pixels in both
  modes, keeping the camera's aspect ratio (160 × 90 at 16:9). Live frames are
  downscaled only for analysis.
  Single-level optical flow then runs over a grid of cells of about
  **Cell size** pixels that tiles the whole image (`main/optical_flow.cpp`), with global
  brightness changes removed. Speeds, cell size and smoothing are in analysis-image
  pixels, so a resolution change also changes how much of the scene they cover.
  A cell moves when its speed reaches **Minimum speed** (analysis pixels per second;
  the detector converts it using the measured time between analysed frames, so
  the achieved analysis rate does not change sensitivity). A cell must also shift at
least 0.6 analysis pixels between analysed frames, because sensor noise is per frame:
with the stream off the ESP32-CAM analyses about 10 frames per second, so motion
slower than about 6 px/s is then ignored. Motion is reported when
  **Moving area** percent of cells move (1-30%). Flat cells below **Minimum texture** are
  ignored. **Method** lists the available detectors; optical flow is the first.
- **Show motion in preview** draws the moving cells, MOTION, and REC over the live picture.
- **Recording settings** are in the **SD card recording** tab, above the recordings;
  their Save and Reset to Default affect only those fields, as the Motion detection
  tab's do for detection. Recording needs motion detection on.
  Recording starts after motion lasts **Motion duration** seconds. It includes
  no frames from before the trigger, continues **Keep recording** seconds after
  motion stops, and splits at **Maximum clip length**. Clips use the selected camera
  resolution, at every frame the camera delivers (no rate setting); if the card
  writes slower, frames are skipped until its PSRAM write queue drains. A resolution change starts
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
python tests/webrtc/manual/check_local_webrtc.py --port /dev/ttyUSB0 --browser /path/to/chromium
```

The test resets the board, reads its ephemeral identity over USB, compares the
HTTPS certificate, checks that plain HTTP is closed and the origin rules, signs in with the camera password in a real browser,
decodes camera JPEG, checks that the settings panel shows the camera's controls
as On/Off switches, labelled lists and number fields exactly as its profile
reports, applies every list value and switch state through the panel,
exercises sensor controls and invalid values, inspects
DTLS connection statistics, and reconnects. It writes no credentials or camera
images. Keep serial monitors closed while flashing or running either USB tool.

The full hardware E2E suite (`python -m pytest tests/webrtc/e2e -v -s`) also runs
`tests/webrtc/manual/check_https_reconnect.py` and `tests/webrtc/manual/check_camera_settings.py` after its
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

## Diagnostics

The shared **Diagnostics** tab works in local and remote viewers after camera
password verification. It shows startup/reset reason, Wi-Fi and signaling
connections, viewer/peer events, camera initialization/capture recovery, and
recording errors. **Download logs** saves the displayed history as plain text.
Only selected application messages are collected, never credentials, signaling
payloads, images, or per-frame messages.

The camera retains 48 entries in RAM; reboot clears this history. Entries contain
uptime, severity, and a repeat count. Text is limited to 127 bytes. Each response
contains at most four entries, with a firmware limit of one response every two
seconds (under 1.5 KB per response). The viewer polls every 2.5 seconds while
the page is visible and connected, retains 1,000 displayed lines, and reports gaps
when older entries have expired. Pending controls take priority over log replies.
Logs travel directly over the encrypted camera data channel; camera logs remain in the viewer.

### Saved crash dumps

Panic core dumps are saved to a 256 KB flash partition at `0x3c0000`.
After reboot, Diagnostics reports dump validity, panic reason, crashed task,
program counter, and the crashing firmware's ELF SHA256 when available.
The latest dump remains until a later panic replaces it. Power loss and hard
hangs may leave no dump, and an interrupted dump write can leave a corrupt dump.
PSRAM contents are excluded by the ESP-IDF core dump implementation.

For detailed analysis, preserve the matching `webrtc.elf` from the build and
read the dump partition using esptool `read-flash 0x3c0000 0x40000 core.bin`.
Use ESP-IDF's `idf.py coredump-info` or `idf.py coredump-debug` with the matching
project build to decode task stacks. Dumps contain memory and should be treated
as private diagnostic files.

Downloaded diagnostic reports include board, firmware version, ELF hash, ESP-IDF
version, reset reason, and browser export time. Main events include Wi-Fi
disconnect reason/RSSI, signaling and peer failures, settings saves/resets,
recording start/stop/errors, SD mount/format failures and space reclamation.
Internal RAM free/largest/minimum and Wi-Fi RSSI are sampled once per minute.
Device history remains bounded to 48 events, and reports identify history gaps.
**Clear logs** empties the panel and, when connected, the camera's event
history; the camera then records that a viewer cleared it. Event ids keep
increasing, so a clear is never reported as a history gap.

The shared Diagnostics panel provides **Download crash summary** after
authentication in both local and remote viewers. It saves
`camera-crash-summary.txt`: the crashed firmware's ELF hash (and whether the
running build matches), the crashed task, exception cause, faulting address,
program counter and backtrace, plus an `addr2line` command to decode them with
the matching archived `webrtc.elf`. The raw dump never leaves the camera over the
network: its task stacks can hold keys, passwords, signaling URLs and IP
addresses. CPU registers are left out of the summary for the same reason. The
saved dump stays in flash; a missing dump is reported in the panel.
