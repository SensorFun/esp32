# Device Spaces

A React site with a general project collection and independent project spaces.
The home page uses a light theme; the WebRTC space owns a dark camera workspace,
navigation, connection guide, and project information.

## Develop

```sh
cd site
npm ci
SIGNALING_URL=wss://signal.example.com/viewer npm run dev
```

Use the local address printed by Vite. Node 18, 20, or 22+ is supported; the deployment
workflow uses Node 22. Set SIGNALING_URL to your public viewer endpoint; no secret is needed to build the site.

```sh
SIGNALING_URL=wss://signal.example.com/viewer npm run build
npm run preview
```

## GitHub Pages

The workflow at `.github/workflows/site-pages.yml` installs locked dependencies,
builds `site/dist`, and publishes that directory to GitHub Pages on changes to
`site/` on `main`, or when manually dispatched.

1. In the repository's **Settings → Pages**, choose **GitHub Actions** as source.
2. Set the repository Actions variable `SIGNALING_URL` to your public `wss://` endpoint ending in `/viewer`.
3. Push the changes to `main`, or run the deployment workflow manually.
4. Use the URL reported by the workflow's deployment environment.

The site has not been published by adding these files. A deployment requires
pushing the changes and enabling Pages in the repository.

Routes use the URL hash, so refreshing or sharing a project URL works on static
hosting without server rewrite rules. Asset URLs are relative, supporting both
repository subpaths and custom-domain roots:

- `/#/` — general home
- `/#/spaces` — project collection
- `/#/about` — about the collection
- `/#/spaces/webrtc` — WebRTC workspace
- `/#/spaces/webrtc/setup` — connection guide
- `/#/spaces/webrtc/notes` — project information

For a repository site, these hashes follow the repository's base URL, such as
`/<repository>/#/spaces/webrtc`.

## Add a project space

Add its React component and scoped stylesheet under `src/spaces/<id>/`, then
register its ID, title, description, icon, and component in `src/spaces/index.jsx`.
Each component receives its remaining route as `subpage` and owns its interface.
The home/index pages render the registry automatically.

## WebRTC connections

The workspace defaults to a private Janus server. Follow the
[local Janus and Cloudflare Tunnel guide](../tools/signal_server/README.md) to run the
signaling server on your computer. Users enter only the camera MAC and camera
password chosen during setup. The public viewer endpoint is configured when
building the site; the Janus API secret stays on the server. No credentials are
embedded in the public build or saved to browser storage.

The same frontend runs on GitHub Pages, which serves static files only. Both
camera and browser connect through a viewer gateway and Cloudflare Tunnel; WebRTC images and
commands travel directly between them. TURN remains disabled. The camera must
be configured and flashed with Janus support using the `/camera` URL and server secret.

Choose the camera's signaling server during Wi-Fi provisioning: the built-in
`signalserver.sensorfun.net` service or a compatible custom server. Configure this
site's viewer endpoint to use the same service.

The local-camera form opens the board's original HTTPS viewer in a separate tab.
It does not fetch the board API across origins. The remote-signaling flow also
makes no board HTTP requests: JPEG frames and settings use encrypted WebRTC.

## Browser verification

With the production preview running, the optional Playwright smoke test checks
navigation, project theme changes, refresh/history, unknown routes, mobile
layouts, and the local-camera link. It never contacts a real camera.

```sh
python scripts/check-browser.py --url http://127.0.0.1:4173/ --browser /path/to/chromium
```

The test needs Python with Playwright installed. It also works against a
repository-subpath preview URL.
