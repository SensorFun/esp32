# Local Janus signaling through Cloudflare Tunnel

This stack runs Janus on your computer and exposes its WebSocket API through a
locally managed Cloudflare Tunnel. The React viewer can remain on GitHub Pages.

```text
GitHub Pages viewer ── WSS ── Cloudflare Tunnel ── Gateway ── Janus
                                                  │
ESP32 camera ───────── WSS ── Cloudflare Tunnel ─────┘
      ╰──────── direct encrypted WebRTC ─────── viewer
```

The included `janus.plugin.signaling` plugin forwards offers, answers, ICE
candidates, and camera readiness messages between two peers. It deliberately
does not negotiate JSEP or terminate WebRTC at Janus. The standard Janus call
plugins relay media and would require reachable media ports beyond the tunnel.
Here, no media ports or router port forwarding are required on the server.
STUN-only peer connectivity still depends on the camera and browser networks;
restrictive NAT/firewalls may prevent a connection. TURN remains disabled.

## 1. Create the Cloudflare Tunnel

In the Cloudflare dashboard, add your domain to Cloudflare and configure its
nameservers if you have not already done so. Use the credentials JSON for your
existing locally managed tunnel, following the
[Cloudflare setup guide](https://developers.cloudflare.com/tunnel/features/locally-managed-tunnels/create-local-tunnel/).
Keep that file in the ignored `.secrets/` directory beside this README. Do not
commit it. The launcher automatically selects the only JSON file in that directory;
use `--credentials /path/to/credentials.json` if you have multiple files.

Your public hostname must have a proxied CNAME pointing to
`<tunnel-uuid>.cfargotunnel.com`. The local `cloudflared.yaml` forwards traffic
received by this tunnel to `http://gateway:8080`. The gateway exposes `/viewer`
for browser connections and `/camera` for authenticated firmware. All services share their
dedicated Compose network. Cloudflare terminates public TLS; browsers and
cameras use your `wss://` hostname. This stack exposes no Janus administration API.

## 2. Start the stack

Generate a random signaling secret, and save it in your password manager:

```sh
python3 -c 'import secrets; print(secrets.token_urlsafe(32))'
```

From the repository root, read the Janus secret into the launcher environment
without putting it in shell history:

```sh
read -rsp 'Janus signaling secret: ' JANUS_API_SECRET
export JANUS_API_SECRET
python3 tools/signal_server/run.py
```

The launcher validates the Janus secret and the tunnel credentials JSON. The
launcher also accepts `--api-secret-file /path/to/key`, or reads the ignored
`.secrets/janus_api_secret` file when `JANUS_API_SECRET` is not set. Keep this file
owner-readable only. This supports restarting the stack without re-entering its
device/server key.

The Cloudflare JSON is provided only to Cloudflared as a read-only Docker Compose
secret at `/run/secrets/cloudflare_credentials`. The tunnel UUID is read from
that JSON; no tunnel token environment variable is used. Cloudflared runs with
the launcher's UID/GID so owner-only credential files remain readable without
loosening their permissions. Compose secrets do not encrypt the host file.

The Janus secret must be 32–128 characters
using letters, digits, underscores, or hyphens. Docker builds Janus v1.4.3 and
the custom plugin. The API secret is required on every Janus API request. It is
rendered into a private configuration file on container tmpfs at startup; it is
never baked into the image. The Docker build context contains only the build
inputs, even if you have local configuration files beside them.

No container ports are exposed or published on the host. The Python launchers
and plugin source are mounted read-only when needed; only compiled Janus and
plugin binaries are stored in the image.
The Cloudflare connector uses the internal Docker network. Your computer must
stay awake and connected to the Internet. With the same environment set:

```sh
python3 tools/signal_server/run.py logs
python3 tools/signal_server/run.py down
```

## 3. Configure and flash the camera

The existing camera firmware needs the new Janus client; previously flashed
Espressif-only firmware will not connect to this server automatically.
Configure the appropriate board, for example:

```sh
./docker/docker.sh esp32 menuconfig --project webrtc --board esp32s3n16r8_espressif_cam
```

Under **Camera remote signaling**, set:

- **Janus API secret**: the same secret used by the launcher.
- **STUN URL**: retain the configured STUN service or choose your own.

Build and flash using the matching device profile:

```sh
python3 tools/build.py project=webrtc device=esp32s3_wroom_ov3660_62
python3 tools/flash.py project=webrtc device=esp32s3_wroom_ov3660_62
```

Menuconfig stores these values in ignored `build/webrtc/<board>/sdkconfig`.
Keep this file and firmware binaries private: the signaling secret is embedded
in firmware. Do not put credentials in tracked `sdkconfig.defaults`.

On the camera's Wi-Fi provisioning page, choose **signalserver.sensorfun.net**
(the default), or **My own signaling server…** and enter your custom secure URL
ending in `/camera`. The selection is stored in NVS after a successful Wi-Fi
connection and survives reboot. A device with no saved selection uses
`wss://signalserver.sensorfun.net/camera`. The chosen server must accept the
device API secret configured in firmware.

## 4. Connect from GitHub Pages

Set the repository Actions variable `SIGNALING_URL` to your public
`wss://signal.example.com/viewer` endpoint, then build/deploy through the existing
GitHub Pages workflow. For a local build, export the same public URL before
running `npm run build --prefix site`. The build launcher validates this setting;
no Janus secret is passed to the site build.

Users open **WebRTC → Workspace**, enter only their camera MAC and camera
password, and connect. These derive a private room capability in the browser.
The viewer gateway attaches only to the signaling plugin, forces the viewer
role, binds all requests to its own Janus session/handle, and supplies the
server API secret internally. Viewers cannot register cameras or select other
plugins. A viewer can join only an existing camera room. A wrong MAC/password
derives a different room and is rejected before signaling reaches the camera.
The camera separately verifies its password over the encrypted WebRTC data
channel before allowing frames or controls.

The API secret remains an operator/device credential. It is required by the
camera firmware on `/camera` and by the gateway's connection to Janus, and is
never sent to browsers. Rotate it by recreating the stack and updating camera
firmware. Each room permits one camera and one viewer; room IDs and passwords
are never saved to browser storage.

## Validation

```sh
docker build -t esp32-signaling:local tools/signal_server
npm ci --prefix site
node --test tools/signal_server/test.mjs
npm test --prefix site
SIGNALING_URL=wss://signal.example.com/viewer npm run build --prefix site
```

The integration tests start a disposable local Janus container with random
credentials. They exercise rejected credentials, the real browser adapter,
offer/answer and ICE forwarding, role checks, private rooms, duplicate viewers,
cleanup/rejoin, rejection of missing camera rooms, and gateway role restrictions.
The test services share a container network namespace without publishing ports.
They do not create a Cloudflare tunnel, publish GitHub Pages, or test a physical
camera connection.

## Live USB camera test

`tools/test_janus_e2e.py` serves the production site locally while all signaling
uses the public Cloudflare hostname. It checks wrong-password rejection,
unauthenticated commands, replay rejection, decoded JPEG frames, camera controls,
direct ICE transport, and reconnect on each board. No Docker ports are published.

Before running, flash matching device/server API keys and provision working Wi-Fi
on both boards. The test expects each board's current camera password in the
private `.secrets/hardware/esp32s3-camera-password` or
`.secrets/hardware/esp32-camera-password` file; it never changes those passwords
or Wi-Fi settings. With the signaling stack running:

```sh
.venv/bin/python tools/test_janus_e2e.py \
  --ports /dev/ttyUSB0 /dev/ttyUSB1 \
  --server signalserver.sensorfun.net \
  --browser /path/to/chromium
```

If provisioning is needed, `tools/provision.py --camera-password-file` can read
one of these private camera password files while prompting for Wi-Fi credentials
locally. Do not send Wi-Fi passwords in chat or put them in command arguments.
