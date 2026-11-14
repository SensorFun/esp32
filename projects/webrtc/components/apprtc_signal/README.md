# Espressif AppRTC adapter

Adapted from esp-webrtc-solution commit dc3ab928137916d7b214a9a8ffe52e0e81794471:
`components/esp_webrtc/impl/apprtc_signal`, its signaling wrapper, and public header.
Original license headers are preserved. Local changes remove credential/payload
logging and select only STUN when the service supplies both STUN and TURN URLs.
The camera always answers browser offers, independently of room-join order.
TURN credentials are discarded; media relaying is disabled.
The component is isolated to avoid pulling in unrelated audio/render pipelines.

The adapter persists only its own room/client membership in NVS and removes that
previous membership before rejoining after a reboot. It never removes a viewer's
membership. This avoids filling a stable MAC room with stale camera entries after
an abrupt restart. External clients can still occupy the public service's slots.

SDP answers/candidates use the registered secure WebSocket channel, avoiding
a separate HTTPS request for each answer. Join/leave and ICE configuration
continue to use Espressif's HTTPS endpoints.

A failed HTTP leave keeps the saved membership even when collider deletion
succeeds, allowing startup to retry both cleanup operations.
