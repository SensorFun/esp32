# WebRTC tests

Run the firmware and protocol checks without hardware:

```sh
PYTHONPATH=python python -m pytest tests/webrtc --ignore=tests/webrtc/e2e
npm test --prefix site
```

Browser unit tests are included in the site test command. The signaling
integration suite uses Docker and is documented in the
[signaling service README](../../tools/signal_server/README.md#validation).
The `manual/` folder contains standalone checks for connected cameras.

## Board end-to-end tests

Run from any directory:

```sh
/path/to/esp32/tests/webrtc/boards_test.sh
```

The launcher uses `uv` to provide Python, pytest-xdist, browser automation,
esptool and Node dependencies. Docker must be running, and Chromium must already
be installed at `browser` in `tests/webrtc/config/e2e.yaml`.
The built-in SensorFun signaling service must be reachable.

Each configured device gets its own worker. The workers run concurrently through
firmware build, USB flash, fresh USB-only provisioning (the board must wait for
USB setup without opening a setup access point, also after a failed join),
public-signaling streaming and controls, local camera password checks (plain
HTTP closed; no frames or settings without authentication; wrong password
rejected), three HTTPS reboot/reconnect cycles, and three randomized Save/reboot
rounds plus reboot checks for unsaved changes, live reset and saved defaults. Operations
on an individual board remain sequential. A failure on one board does not cancel
the other board; the launcher exits unsuccessfully if any board fails.
Each run prints the path to its full log and JUnit report under ignored
`build/board-tests/`. Failure summaries name the failed stage; the log retains
the detailed browser and USB diagnostics. Report directories are private.
Workers also write a private `*.failure.log` immediately when a stage fails,
so its details are available while the other board is still running.
Local browser tests allow the board's self-signed certificate in Chromium's
browser and context settings, preventing certificate-warning retries during
repeated reboots while exercising HTTPS asset loading and encrypted WebRTC.

Edit `tests/webrtc/config/e2e.yaml` to select devices. Devices must use distinct board
build directories and USB ports. Board ports are detected automatically. If several adapters match, pass
`--port DEVICE=/dev/serial/by-id/...` for each ambiguous device. The launcher locks each board against another launcher run. Close serial monitors
and other flashing tools before running.

Configure signaling in each `build/webrtc/<board>/sdkconfig` beforehand, matching
the built-in SensorFun signaling service. Builds run in separate Docker containers,
mount source read-only, and keep managed components, dependency locks and outputs
in each board's build directory. Containers publish no ports. Each worker builds
the website into a private temporary directory and serves it on its own host
loopback port; tests do not modify the development website's `site/dist` output.

Wi-Fi credentials come from `E2E_WIFI_SSID` / `E2E_WIFI_PASSWORD`, or ignored
`tests/.secrets/wifi_ssid` / `tests/.secrets/wifi_password` files. If necessary,
the launcher prompts once before starting workers. Credentials are never passed
in command-line arguments. The tests replace board provisioning and use random
camera passwords held only in memory. Automated USB provisioning reads its
credentials from stdin without opening a terminal or printing prompts.
Provision your preferred camera password
again after testing. Camera settings are restored at the end of persistence
tests.

Useful commands:

```sh
tests/webrtc/boards_test.sh --collect-only
tests/webrtc/boards_test.sh --device <configured-device>
tests/webrtc/boards_test.sh --help
```

`--collect-only` does not access USB, Docker or credentials.

## Offline motion replay

Run from the repository root:

```sh
python tests/webrtc/replay_motion.py --jobs 6
python tests/webrtc/replay_motion.py --matrix tests/webrtc/config/motion-replay.json --jobs 6
```

The default inputs are the camera's MJPEG AVI files in `tests/data/motion/`.
Pass clip paths to select other fixtures. The runner compiles the firmware's
C++ detector and the same pinned ESP-JPEG decoder with the host C and C++
compilers. A firmware build provides the decoder sources. For a different
build layout, pass the pinned component's `tjpgd` directory with
`--decoder-dir`.

The default profiles use firmware motion thresholds with detection enabled.
For a board-specific profile, use its `motion_get` JSON response or the
response's `values` object in the matrix's `profiles` map. Missing thresholds
use firmware defaults. The matrix runs every clip × profile × scenario in a
separate process, up to `--jobs` concurrent cases. Scenarios can repeat the
first frame (`freeze`), keep every Nth frame (`frame_stride`), and simulate
analysis taking a fixed number of microseconds (`work_us`). Set
`expect_motion` to assert detection or its absence. Omit it for exploratory
scenarios that should report results without requiring a detection.

The report defaults to `build/motion-replay/report.json`. It includes admission,
timestamps, analysis dimensions, decode failures, motion area and speed,
and the first sampled point that reaches the recording confirmation threshold.
This checks the detector and confirmation logic rather than writing recordings
or simulating SD card I/O. AVI supplies a constant average frame interval, so
replay cannot reconstruct exact capture jitter or busy periods from the
original run. The host's actual execution time never feeds the detector.

To export annotated MP4s, add `--video-output`:

```sh
uv run --no-project --with 'pillow>=10.1' --with imageio-ffmpeg python \
  tests/webrtc/replay_motion.py --video-output output/motion --jobs 6
```

The runner writes one MP4 per clip, profile, and scenario. Green boxes enclose
connected groups of cells marked as moving when the detector reports motion.
Between analyzed frames, the overlay holds the previous result and labels it
`(held)`. Rendering and encoding happen after detection and do not affect
the results. The export needs Pillow and an FFmpeg build with H.264 encoding.
