# ESP32 workspace

The [React project site](site/README.md) provides a general home and dedicated
project spaces, ready to deploy with GitHub Pages.
For private remote access, run the [local signaling stack](tools/signal_server/README.md)
through Cloudflare Tunnel and connect from the GitHub Pages WebRTC workspace.
See the [STUN-only end-to-end test guide](site/scripts/README.md) to run the
React viewer locally with Espressif public signaling and the attached cameras.

Python host tooling and native ESP-IDF C/C++ firmware share one repository.
Python packaging does not compile firmware. Firmware applications under
`projects/` are independent ESP-IDF CMake projects; `projects/common/` contains
shared components.

- `python/esp32_workspace/`: build, flash, and serial-monitor CLI.
- `projects/webrtc/`: [local encrypted WebRTC camera, sensor controls, motion detection, and SD recording](projects/webrtc/README.md).
- `projects/common/application/`: shared setup/main lifecycle and board-specific NVS reset.
- `projects/common/provisioning/`: reusable Wi-Fi setup page and credential storage.
- `boards/`: pin headers and USB programming identities.
- `config/boards/`: manufacturer dictionaries of board names, header paths, chip targets, and supported frame grabbers.
- `cmake/board.cmake`: shared board selection for native firmware projects.
- `docker/`: base and development Docker images and Compose.
- `config/`: Hydra firmware build and flash configuration; `config/firmware/` lists flashable firmware.
- `tools/`: configuration-driven firmware build and flash entry points.

## Local use

Install ESP-IDF v6.1 and source its `export.sh`, then run from this directory:

```sh
python -m pip install -e .
esp32 list
esp32 build --project webrtc --board esp32_ai-thinker_cam
esp32 flash --project webrtc --board esp32_ai-thinker_cam --port /dev/ttyACM0
esp32 monitor --project webrtc --board esp32_ai-thinker_cam --port /dev/ttyACM0
```

Use `--dry-run` to inspect a command without compiling or accessing hardware.
Use `--root /path/to/workspace` (or `ESP32_WORKSPACE`) outside the repository root.
Build artifacts and generated sdkconfig files are isolated in
`build/<project>/<board>/`, allowing the same firmware to target multiple boards.
`menuconfig` and `clean` are also supported. Exit the serial monitor with Ctrl+].
When upgrading ESP-IDF, move aside the affected `build/<project>/<board>/`
directory before rebuilding so CMake detects the new compiler toolchain.

## Commit checks

Install the development tools in your virtual environment and activate the hooks
once per clone:

```sh
python -m pip install -e '.[dev]'
pre-commit install
pre-commit run --all-files
```

Commits check whitespace, YAML/JSON/TOML syntax, Python syntax, merge conflicts,
large files, private keys, and secrets with Gitleaks. The first run downloads the
hook environments. Gitleaks scans staged changes when committing, including when
invoked through `pre-commit run --all-files`. To scan existing Git history, use
`gitleaks git --redact` separately with an installed Gitleaks binary.

Hooks must be installed in each clone and can be bypassed. Secret detection is
heuristic, so keep real credentials in ignored local configuration files.

## Configured build and flash

Run `python3 tools/build.py` to compile in Docker automatically. The build host
only needs Python 3 and Docker Compose v2; Hydra and ESP-IDF are in the image.

Flashing runs locally with Python, Hydra, pySerial, and esptool. Install the host
package once in a virtual environment; the full ESP-IDF toolkit is not needed:

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -e '.[flash]'
python tools/build.py
python tools/flash.py
```

`tools/build.py` reads `config/build.yaml`; `tools/flash.py` reads
`config/flash.yaml`. Both inherit `config/firmware.yaml`, selecting `webrtc`
and its board/camera device profile. The default is `esp32_cam`.
Select either connected device:

```sh
python tools/build.py device=esp32_cam
python tools/flash.py device=esp32_cam
python tools/build.py device=esp32s3_wroom
python tools/flash.py device=esp32s3_wroom
# Build both profiles in sequence:
python tools/build.py --multirun device=esp32_cam,esp32s3_wroom
python tools/flash.py port=/dev/ttyACM0 baud=460800
python tools/flash.py port=/dev/ttyACM0 dry_run=true
```

The profiles live in `config/device/` and compose `board`, `frame_grabber`, and
`lens` directly. OV devices use `config/frame_grabber/ov.yaml`; the S3-WROOM device uses
`config/frame_grabber/gc.yaml` for its attached GC0308. All select
`config/lens/general.yaml`, currently an empty lens profile. Firmware detects
the physical sensor and applies the corresponding profile and supported controls.
The device selects `frame_grabber=ov` or `frame_grabber=gc`; board entries list supported profile names in `supported_frame_grabber`
that must exist under `config/frame_grabber/`. Sensor identification is automatic
in firmware; there is no default camera selection.
Hydra board metadata validates frame grabber selection, and each build records its
selection in `firmware_selection.json`; the host flasher rejects mismatched
frame grabber profiles. USB adapter identities select the connected board without
storing unique device serial numbers. With multiple identical adapters, set a
port explicitly. The S3 profile uses the existing N16R8 WROOM camera board header;
"WROOM" alone does not verify a carrier's camera wiring.

GC0308 captures RGB565 and uses software JPEG encoding for the existing WebRTC
viewer. Its resolutions are limited to VGA (640 × 480). `gc.yaml` contains only
implemented sensor controls; OV light timing, hardware JPEG quality, brightness,
denoise, gain ceiling, AWB gain, AEC2, downsize, pixel correction, raw gamma, and
lens correction are not exposed as supported GC0308 controls. Sharpness uses
its GC0308 range of −2 to +2. Resolution changes reconfigure raw DMA buffers;
Save and Reset retain the same per-sensor persistence behavior as OV cameras.

The GOOUUU Tech ESP32-S3-CAM N16R8 profile is `device=esp32s3_goouuu`:

```sh
python tools/build.py project=webrtc device=esp32s3_goouuu
python tools/flash.py project=webrtc device=esp32s3_goouuu port=/dev/ttyUSB0
```

Its camera and microSD pins match the existing S3 header, so the board entry
shares that header. It supports both `ov` and `gc` frame grabbers; select a GC0308
with `frame_grabber=gc`. Board defaults select 16 MB flash and octal PSRAM. The
[GOOUUU V1.5 supplier guide](https://www.phippselectronics.com/support/esp32-s3-cam-ov2640-2mp-wifi-camera-development-board-users-guide/)
documents the pin mapping. The [board image source](https://www.phippselectronics.com/wp-content/uploads/2026/11/PHI1001972-ESP32-S3-CAM-OV2640-2MP-WiFi-Camera-Development-Board-03.jpg)
was downloaded unchanged into `config/boards/images/esp32s3n16r8_goouuu_cam.jpg`.
Use an explicit `port=` when multiple boards are connected; a GOOUUU-specific
USB identity has not been verified. This profile does not add OV5640 sensor
support; the shared OV firmware currently recognizes OV2640 and OV3660.

Flash reads the binaries, flash addresses, and settings generated by ESP-IDF in
`build/<project>/<board>/`. It requires an existing build and never compiles or
starts Docker. Port detection uses USB identities for Espressif and common serial
bridges, plus `config/device/*.yaml` to identify known boards. If several
ports remain ambiguous, select one with `port=...`; if none are detected, connect
the board or provide its port explicitly.

For the browser flasher, `config/firmware/<project>.yaml` lists which
projects are flashable and for which boards. `python3 tools/firmware_artifacts.py`
builds and packages every listed pair into `build/firmware/`; CI runs the same
script. See [the site README](site/README.md#flasher).

Both tools preserve failure exit codes. `dry_run=true` prints the command without
building firmware or flashing; build dry runs still start Docker. Flash dry runs
require build artifacts and either a detected or explicit port. Use `--cfg job`
to inspect configuration. Hydra output stays under `build/hydra/`. Tools locate
the workspace independently of the current directory.

## Wi-Fi provisioning

WebRTC uses the shared [provisioning component](projects/common/provisioning/README.md).
Without saved settings, join the board's open `CAM_<MAC>` Wi-Fi (no password
or serial access needed), then open `http://192.168.4.1`. Refresh to
scan networks, choose or enter an SSID, and save its password. Credentials persist
in board NVS after a successful connection; setup closes afterward. A failed saved
connection reopens setup after 20 seconds.

[The common application class](projects/common/application/README.md) owns
`setup()`, `main()`, and `enter_setup()`. Its reset wiring comes from board headers:

- **ESP32-CAM:** connect **IO12 to GND** for **two seconds**, then disconnect.
- **ESP32-S3:** hold **BOOT** for **two seconds while running**, then release.
- Serial `wifi-reset` and `common.enter_setup()` also request setup.

A setup reset clears the entire default NVS partition on the next software boot,
including Wi-Fi and other saved application settings. A normal reboot preserves
settings. IO12 is reserved for this trigger; do not use it simultaneously for SD
or connect U0T/UART TX to ground. No real credentials belong in this repository.

## Adding firmware and boards

Create an ESP-IDF project under `projects/` with its own CMake project name and
`main/` component. See the [common application component](projects/common/application/README.md)
for an example of integrating shared Wi-Fi provisioning.
Add a board entry to `config/boards/<manufacturer>.yaml`, keyed by its
unique board ID. For example:

```yaml
esp32_ai-thinker_cam:
  name: AI Thinker ESP32-CAM
  header: esp32_ai-thinker_cam.h
  image: images/esp32_ai-thinker_cam.png
  target: esp32
  supported_frame_grabber: [ov]
```

`header` is relative to `boards/`; several entries may share one header.
`image` is relative to `config/boards/` and must be a file in `config/boards/images/`. Headers
contain pin assignments and `common_board_config` for reset wiring and Wi-Fi
capability. Use `board_display_name` for its `.name`; CMake generates that value
from YAML in a build-local wrapper, then includes the selected pinout header.
Physical headers do not define display names.

Device profiles select a `board` ID and compose frame grabber and lens profiles.
The firmware defaults compose manufacturer catalogs into `board_catalog` and
resolve `hardware` from the selected board. Add new manufacturer filenames to
that defaults list. Use `device=esp32_cam` or `device=esp32s3_wroom` as before;
`board=<id>` overrides work across manufacturers. Build folders remain keyed by
board ID, even when headers are shared.

Python tools and CMake read the same manufacturer YAML; there are no separate
board JSON files. Duplicate IDs, missing headers, invalid metadata, and paths
outside `boards/` fail before compilation. For native CMake use, install the
Python package in the active ESP-IDF Python environment so the resolver can read
YAML. Docker already does this. Optional `sdkconfig_defaults` entries in a board
entry name files under `boards/`.

Firmware builds use native ESP-IDF. Project
`sdkconfig.defaults` loads first, followed by any defaults explicitly listed in
the board metadata.
Changes to defaults affect newly generated sdkconfig values; use `menuconfig`
for an existing configuration or remove that board's build directory to reset it.
Device profiles supply known USB identities for host flash detection.
The lower-level `esp32 flash` CLI still delegates to ESP-IDF.

## Docker

The base and development images provide ESP-IDF v6.1 and host tooling for
running commands. Use Docker Compose v2 with additional build context support.
Building and running commands do not require a board.

```sh
./docker/docker.sh --build                  # Build the development image
./docker/docker.sh esp32 list               # Run the workspace CLI
./docker/docker.sh esp32 build              # Compile firmware
./docker/docker.sh python --version         # Run any command
./docker/docker.sh --build esp32 build      # Build the image, then compile
```

Each invocation runs the supplied command in a temporary container and removes
it when the command exits, returning the command's exit status. Arguments pass
through unchanged; use `bash -c 'command1 && command2'` for shell expressions.
The repository and build artifacts stay on the host through the workspace mount.
The launcher derives the current host user's username, UID, and GID directly.
The image includes the Python CLI and imports the mounted workspace sources, so
Python edits take effect without rebuilding. Rebuild for dependency or packaging
changes. Supply a command or `--build`.

Run host tests without ESP-IDF or hardware:

```sh
PYTHONPATH=python python3 -m unittest discover -s tests
```
