# SensorFun ESP32

The ESP32 project space of [SensorFun](https://sensorfun.net/), served at
`https://esp32.sensorfun.net/`. The SensorFun landing page lives in the separate
main repository and links here. This site's home page lists the ESP32 projects;
each project, such as WebRTC, owns its own workspace. The SensorFun logo returns
to `https://sensorfun.net/`, and the **ESP32** breadcrumb returns to the project list.

## Develop

```sh
cd site
npm ci
npm run dev
```

Use the local address printed by Vite. Node 18, 20, or 22+ is supported; the deployment
workflow uses Node 22. The site uses `wss://signalserver.sensorfun.net/viewer`; no secret is needed to build the site.

```sh
npm run build
npm run preview
```

## GitHub Pages

The workflow at `.github/workflows/site-pages.yml` installs locked dependencies,
builds `site/dist`, and publishes that directory to GitHub Pages on changes to
`site/` on `main`, or when manually dispatched.

1. In the repository's **Settings → Pages**, choose **GitHub Actions** as source.
2. Set the custom domain to `esp32.sensorfun.net`, and add a DNS `CNAME` record
   for `esp32` pointing to the GitHub Pages host of this repository's owner.
3. Push the changes to `main`, or run the deployment workflow manually.

The signaling endpoint is built in; no repository signaling URL variable or
secret is required.

Routes use the URL hash, so refreshing or sharing a URL works on static hosting
without server rewrite rules. Asset URLs are relative, supporting both
repository subpaths and custom-domain roots:

- `/#/` — ESP32 project list
- `/#/projects/webrtc` — WebRTC camera workspace
- `/#/tools/flasher` — browser firmware flasher (`?project=<id>` selects a project)

Other hashes show a page-not-found screen with links to the project list and to
SensorFun.

## Add a project

Add its React component and scoped stylesheet under `src/projects/<id>/`, then
register its ID, name, category, description, and component in
`src/projects/index.jsx`. The home page renders the registry automatically. Also
add the project to the ESP32 card on the SensorFun landing page in the main
repository.

## WebRTC connections

The workspace defaults to a private signaling server. Follow the
[local signaling and Cloudflare Tunnel guide](../tools/signal_server/README.md) to run the
signaling server on your computer. Users enter only the camera MAC and camera
password chosen during setup. The public viewer endpoint is fixed to
`wss://signalserver.sensorfun.net/viewer`; the signaling API secret stays on the server. No credentials are
embedded in the public build or saved to browser storage.

The same frontend runs on GitHub Pages, which serves static files only. Both
camera and browser connect through the signaling server and Cloudflare Tunnel; WebRTC images and
commands travel directly between them. TURN remains disabled. The camera must
be configured and flashed with signaling support using the `/camera` URL and server secret.

Both viewers offer a sensor-specific Resolution selector: all driver sizes up to
1600×1200 for OV2640 and 2048×1536 for OV3660, starting at 160×120.
Both viewers reuse `projects/common/camera_ui/settings.js`. Setting changes take
effect immediately. Select **Save** to keep them in the camera's nonvolatile
storage. Saved settings are restored after reboot. **Reset to Default** applies and saves the configured
sensor defaults, including 320×240 resolution. Unsaved changes are lost on reboot;
resolution changes retain the other camera controls and light timing profile.
The website remembers the last valid camera MAC in a cookie for one year and
prefills it on return. Clearing the MAC field removes that cookie. Camera
passwords and authentication keys are never stored in it.
FPS reports displayed frames over each one-second sampling interval. Streaming
has no fixed 5 FPS cap; achievable FPS depends on resolution, exposure, and the
direct connection. Larger images can take longer to transfer.

Choose the camera's signaling server during Wi-Fi provisioning: the built-in
`signalserver.sensorfun.net` service or a compatible custom server. Configure this
camera to use the same service as the site's fixed public endpoint.

The local viewer link is shown on the board's Wi-Fi provisioning page after it
receives its network IP address. Setup closes after about 30 seconds; reconnect
to the camera's Wi-Fi network and open that HTTPS link. The website uses remote
signaling and makes no board HTTP requests: JPEG frames and settings use
encrypted WebRTC.

## Flasher

`/#/tools/flasher` writes firmware to a board over USB entirely in the
browser, using Web Serial and Espressif's [esptool-js](https://github.com/espressif/esptool-js).
It needs Chrome or Edge on a computer, over HTTPS or localhost; other browsers
see an explanation instead. The older `/#/projects/flasher` address still works.

What it offers comes from two catalogs, read at build time by
`scripts/boards-plugin.mjs`, so adding an entry needs no site changes:

- `config/firmware/<project>.yaml`: each flashable project, its boards, and
  what to do after flashing ([fields](../config/firmware/README.md)).
- `config/boards/*.yaml`: each board's `name` and `image`.

Packages come from `build/firmware/<project>/<board>/`, produced by
`tools/firmware_artifacts.py` locally and in CI. `#/tools/flasher?project=<id>`
opens one project directly, so a project page can link to its firmware.

1. **Firmware**: pick a project under **Firmware source**, then its board
   card; boards without a package show *No firmware yet*, and a ready package
   offers **Download firmware file** (a single full image). Or choose
   **Upload your firmware** and select one `.bin`, read in the browser and
   sent only to the board. The page reads its ESP image header:
   - an app image (for example `build/webrtc/<board>/webrtc.bin`) keeps the
     board's bootloader and partition table, so the board must already have
     them; it is written at its project's app offset when the image names a
     catalog project, otherwise 0x10000, and full erase is unavailable;
   - a full flash image (`esptool merge-bin`, or a downloaded
     `firmware.bin`) is written at its bootloader offset (0x1000 for ESP32 and
     ESP32-S2, 0x0 for most others, or 0x0 when the file starts with erased
     padding) and replaces everything.

   Other files are rejected. Uploaded files keep the board's flash settings.
2. **Select USB port**: flash one board at a time. The browser's chooser lists
   every serial port; the page shows the selected one by USB vendor (Espressif
   USB, CP210x, CH340, FTDI, Prolific). Selecting another port replaces it and
   forgets the previous one. A single port allowed on an earlier visit is
   selected again automatically while it is connected.
3. **Flash firmware**: the page stops before writing if the connected chip does
   not match the build, verifies every file by MD5 after writing, then
   restarts the board and shows the project's `after_flash` text and link.
   **Erase entire flash first** also clears saved Wi-Fi and settings.

If the board doesn't connect, close other programs using the port, hold BOOT
while starting, or lower the speed.

### Adding a flashable project

1. Create `projects/<project>/` as described in the main README.
2. Add `config/firmware/<project>.yaml` with `name`, `description`, `boards`,
   and optionally `requires_frame_grabber`, `private_settings`, `after_flash`.
3. Check the build list: `python3 tools/firmware_artifacts.py --list`.

CI then builds every listed project and board pair, and the flasher offers it.

### Firmware packages, locally and in CI

CI (`.github/workflows/site-pages.yml`) and local runs use the same script and
the same `espressif/idf:v6.1` image, one container per project and board:

```sh
python3 tools/firmware_artifacts.py --list           # project/board pairs CI builds
python3 tools/firmware_artifacts.py --ci             # exactly as CI: fresh builds, default settings
python3 tools/firmware_artifacts.py                  # reuse your build/<project>/<board> folders
python3 tools/firmware_artifacts.py --project hello_world --board esp32s3n16r8_goouuu_cam
python3 tools/firmware_artifacts.py --package-only   # package existing builds without building
cd site && npm run dev                               # http://127.0.0.1:5173/#/tools/flasher
npm run build && node scripts/check-bundle.mjs dist --all-boards   # CI's bundle check
```

In CI, a job lists the pairs, a matrix job builds each one and uploads
`firmware-<project>-<board>`, and the site job downloads them into
`build/firmware`, runs `npm test`, builds the site with the packages under
`firmware/`, and checks that every pair is bundled. Pull requests stop there;
`main` deploys to GitHub Pages.

A package is private when its `sdkconfig` sets any of the project's
`private_settings` (WebRTC lists its signaling secret). The dev server serves
private packages to this computer only; `npm run build` never bundles them,
and the bundle check fails if one is present. Default builds (`--ci` and CI)
set none, so they can be published; WebRTC builds without a secret cannot join
a signaling server that requires one, while local viewing is unaffected.

## Browser verification

With the production preview running, the optional Playwright smoke test checks
branding, the project list, breadcrumbs, links back to SensorFun, unknown routes,
the flasher (with simulated serial ports, firmware packages, and uploaded files), mobile layouts,
and the camera connection form. It never contacts a real camera.

```sh
python scripts/check-browser.py --url http://127.0.0.1:4173/ --browser /path/to/chromium
```

The test needs Python with Playwright installed. It also works against a
repository-subpath preview URL.
