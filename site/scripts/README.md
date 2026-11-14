# Public signaling end-to-end test

From the workspace root, after building/flashing the `webrtc` project and
provisioning the board Wi-Fi:

```sh
.venv/bin/python tools/run_webrtc_e2e.py --ports /dev/ttyUSB0 /dev/ttyUSB1
```

The launcher builds the static React frontend, discovers stable MAC-derived camera room
IDs over the USB consoles, and starts two processes:

- `http://localhost:5173/#/spaces/webrtc`: the production React build.
- `http://localhost:8787`: a loopback-only signaling bridge.

Open the frontend, click **Load cameras**, select a board, enter its camera password, and click **Connect
camera**. Alternatively, enter the camera MAC directly; USB discovery is optional. No Doorbell website or manually entered room code is needed. **TURN is disabled** on both the browser and firmware. STUN discovers public
addresses; camera images and commands use a direct encrypted connection. Use the camera controls beneath the stream. The board still supports one
peer at a time: disconnect its local viewer before this test.

## Why a bridge?

Espressif's hosted AppRTC join endpoint does not return cross-origin access
headers. A browser served from localhost or GitHub Pages cannot directly read
that response. The bridge performs HTTPS room/ICE requests and forwards WSS
signaling to the browser. It forwards connection metadata only, never camera
JPEG or settings messages. Those use the encrypted WebRTC data channel.

The frontend uses no board IP or board HTTP API in this path. Serving the static
build locally exercises the same browser architecture as GitHub Pages. For a
GitHub Pages deployment, the bridge needs a public HTTPS/WSS endpoint with
appropriate remote access controls and its allowed frontend origin configured;
GitHub Pages cannot run the Node bridge. This launcher intentionally binds the
bridge to loopback for local testing.

Rooms are stable: exactly the Wi-Fi station MAC as 12 uppercase hex digits, without separators.
Both peers use Espressif signaling automatically. The password is chosen during
provisioning and verified by the ESP32 over the encrypted WebRTC data channel
before video/settings are allowed. It is separate from the Wi-Fi password. MAC
addresses identify cameras; they are not secrets. The public service does not
restrict room membership, so knowing a MAC can still allow signaling disruption.

For normal operation without USB, start the bridge directly:

```sh
node site/scripts/signaling-bridge.mjs --state build/e2e/signaling-memberships.json --port 8787 --origin http://localhost:5173
```

`--config` is optional and supplies only the test dropdown. For internet deployment,
put the bridge behind public HTTPS/WSS, pass the GitHub Pages origin to `--origin`,
and enter the public bridge URL in the viewer. The GitHub Pages frontend cannot
connect to the loopback bridge from a remote device. The hosted AppRTC service
remains an external dependency. Saved Wi-Fi and camera passwords survive reboots.

## Automated browser check

While the launcher is running, execute from the workspace root:

```sh
.venv/bin/python site/scripts/check-e2e.py \
  --browser /path/to/chromium \
  --url 'http://localhost:5173/#/spaces/webrtc'
```

It prompts for the camera password, then checks real JPEG decoding, settings, absence of TURN servers and relay candidates, and that the browser
makes no board HTTP requests. No browser CORS/WebRTC security flags are disabled.

The original `check-browser.py` is an offline frontend/navigation smoke test.

## Direct-connection limits

TURN media relaying is deliberately disabled. A successful same-LAN test proves
public signaling and direct encrypted streaming, not compatibility with every
internet NAT. Some routers, symmetric NATs, CGNAT arrangements, or firewalls will
prevent a direct connection. The viewer reports failure instead of enabling a
relay. Test from another network once the signaling bridge is reachable there.
The small AppRTC bridge relays signaling metadata only; it is not a TURN server.

## Authorization tests on test boards

```sh
.venv/bin/python tools/test_remote_access.py --ports /dev/ttyUSB0 /dev/ttyUSB1 --browser /path/to/chromium
```

This replaces any existing camera passwords with randomly generated temporary
passwords, keeps them in memory only, checks wrong passwords, unauthenticated
commands, replayed proofs, correct login/stream/settings, and reconnects, then
clears the temporary passwords. Use test boards only. Wi-Fi settings are preserved.
Choose your permanent camera passwords afterward using `tools/camera_password.py`
or the provisioning page. Local viewing remains unpaired.

The bridge journals only its own room/client membership IDs in the `--state` file.
Keep this file across restarts. Shutdown waits for room and channel removal, and
startup retries unfinished cleanup before admitting viewers. No camera password
or image is stored there.
