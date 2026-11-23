# Browser and camera checks

The website connects to the fixed SensorFun endpoint at
`wss://signalserver.sensorfun.net/viewer`. Camera firmware uses
`wss://signalserver.sensorfun.net/camera`. Remote viewing asks for the camera
MAC and the password chosen during provisioning.

## Run the website locally

From the workspace root:

```sh
npm run dev --prefix site
```

Open the printed local address, select WebRTC, and connect using the camera MAC
and password. The camera must have working Wi-Fi and be connected to SensorFun.
Disconnect other viewers first: each camera supports one viewer at a time.

## Board tests

```sh
tests/webrtc/boards_test.sh
```

This builds, flashes and provisions each configured board in parallel, then
checks remote streaming, live controls, randomized settings persistence across
reboots, and local HTTPS reconnects. It replaces provisioning and uses disposable
camera passwords; see [board tests](../../tests/webrtc/README.md) for details.

## Browser checks

```sh
npm test --prefix site
python3 site/scripts/check-browser.py --url http://localhost:5173/ --browser /path/to/chromium
```

`check-browser.py` checks the frontend, navigation and responsive layouts with
synthetic test fixtures. For an already provisioned camera, a locally served
production build can be exercised through SensorFun:

```sh
python3 tests/webrtc/manual/check-e2e.py \
  --browser /path/to/chromium \
  --url 'http://localhost:5173/projects/webrtc' \
  --mac '<camera-mac>'
```

The live check prompts for the camera password. It checks decoded JPEG frames,
settings, authentication and reconnects without board HTTP requests.

## Direct-connection limits

The signaling service carries connection metadata. Camera images and controls
use the direct encrypted WebRTC data channel. TURN is disabled, so restrictive
NATs or firewalls can prevent a connection. A successful same-LAN test does not
prove connectivity through every internet network.
