# SensorFun signaling service

This directory implements the backend for SensorFun cameras and the public
WebRTC viewer. The service endpoints are fixed:

| Client | Endpoint |
| --- | --- |
| Camera firmware | `wss://signalserver.sensorfun.net/camera` |
| Website viewer | `wss://signalserver.sensorfun.net/viewer` |

Camera setup asks for Wi-Fi credentials and a camera password. Camera and
viewer derive a private room from the camera MAC and password. The camera
also verifies the password over the encrypted WebRTC data channel before
sending frames or accepting controls.

`server.py` forwards SDP, ICE candidates and readiness messages. Images,
recordings and camera controls travel directly between camera and viewer over
WebRTC. STUN-only connectivity depends on their networks; TURN is disabled.

## Service internals

Compose runs the Python relay and the existing Cloudflare connector on a
dedicated Docker network. Containers publish no host ports. Cloudflare
terminates public TLS. Application source and connector configuration are
mounted read-only rather than copied into images.

The API secret is read from `/run/secrets/signaling_api_secret` at startup.
It authenticates camera registrations and is never sent to browsers. The
matching value is an internal firmware build setting. Service credentials
remain in ignored `.secrets/` files and private builds.

`run.sh` manages the existing Compose project and its mounted credentials.
Restarting it stops this project's containers before starting their replacements;
other Compose projects are unaffected.

## Logs

```sh
./tools/signal_server/run.sh logs
```

Connection, rejection and disconnect events include current camera/viewer
counts. A summary is emitted every ten minutes. Room tags are salted per process
and cannot be used to join a room. Logs omit API secrets, complete room IDs and
client addresses. Docker rotates up to five 10 MB log files.

## Validation

```sh
docker build -t esp32-signaling:local tools/signal_server
node --test tools/signal_server/test.mjs
npm test --prefix site
```

Integration tests run disposable containers with random credentials and no
published ports. They check authentication, role restrictions, private-room
membership, offer/answer and ICE forwarding, cleanup, and protocol violations.

Hardware checks use the built-in service:

```sh
tests/boards_test.sh
```

See [board tests](../../tests/README.md) for provisioning, streaming, randomized
settings persistence and reboot coverage.
