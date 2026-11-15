# SensorFan

A React site with a general project collection and independent project projects.
The home page uses a light theme; the WebRTC project owns a dark camera workspace,
navigation, connection guide, and project information.

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
2. The signaling endpoint is built in; no repository signaling URL variable is required.
3. Push the changes to `main`, or run the deployment workflow manually.
4. Use the URL reported by the workflow's deployment environment.

The site has not been published by adding these files. A deployment requires
pushing the changes and enabling Pages in the repository.

Routes use the URL hash, so refreshing or sharing a project URL works on static
hosting without server rewrite rules. Asset URLs are relative, supporting both
repository subpaths and custom-domain roots:

- `/#/` — general home
- `/#/projects` — project collection
- `/#/about` — about the collection
- `/#/projects/webrtc` — WebRTC workspace
- `/#/projects/webrtc/setup` — connection guide
- `/#/projects/webrtc/notes` — project information

For a repository site, these hashes follow the repository's base URL, such as
`/<repository>/#/projects/webrtc`.

## Add a project

Add its React component and scoped stylesheet under `src/projects/<id>/`, then
register its ID, title, description, icon, and component in `src/projects/index.jsx`.
Each component receives its remaining route as `subpage` and owns its interface.
The home/index pages render the registry automatically.

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

## Browser verification

With the production preview running, the optional Playwright smoke test checks
navigation, project theme changes, refresh/history, unknown routes, mobile
layouts, and the camera connection form. It never contacts a real camera.

```sh
python scripts/check-browser.py --url http://127.0.0.1:4173/ --browser /path/to/chromium
```

The test needs Python with Playwright installed. It also works against a
repository-subpath preview URL.
