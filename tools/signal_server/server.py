"""Two-peer signaling relay. No media, no JSEP: SDP/ICE are opaque payloads
relayed verbatim between exactly one camera and one viewer per room.

Activity is logged to stdout (see `docker logs`), one key=value line per event.
Room IDs are join capabilities, so logs show only a short tag salted per
server run; secrets, room IDs and client addresses are never logged."""
import asyncio
import collections
import hashlib
import hmac
import json
import logging
import os
import re
import sys
import time
from pathlib import Path

from aiohttp import WSCloseCode, WSMsgType, web

ROOM_RE = re.compile(r"[0-9A-F]{32}")
ROOM_LIMIT = 128
JOIN_TIMEOUT = 10
MAX_MESSAGE = 32768
MAX_SDP = 16384
MAX_SHORT = 8192
SUMMARY_INTERVAL = 600  # Seconds between activity summaries.

log = logging.getLogger("signaling")


def setup_logging():
    handler = logging.StreamHandler(sys.stdout)
    formatter = logging.Formatter("%(asctime)s %(message)s", "%Y-%m-%dT%H:%M:%SZ")
    formatter.converter = time.gmtime
    handler.setFormatter(formatter)
    log.addHandler(handler)
    log.setLevel(logging.INFO)
    log.propagate = False


def room_tag(app, key):
    """Correlates one room's events within a server run without revealing it."""
    return hmac.new(app["tag_salt"], key.encode(), hashlib.sha256).hexdigest()[:8]


def duration(seconds):
    seconds = int(seconds)
    hours, rest = divmod(seconds, 3600)
    minutes, seconds = divmod(rest, 60)
    return f"{hours}h{minutes:02d}m{seconds:02d}s" if hours else f"{minutes}m{seconds:02d}s" if minutes else f"{seconds}s"


def online(app):
    rooms = app["rooms"].values()
    return (f"cameras_online={sum(pair['camera'] is not None for pair in rooms)} "
            f"viewers_online={sum(pair['viewer'] is not None for pair in rooms)}")


def event(app, name, **fields):
    details = " ".join(f"{key}={value}" for key, value in fields.items())
    log.info("%s %s%s", name, f"{details} " if details else "", online(app))


def reject(app, role, reason, key=None):
    app["stats"][f"{role}_rejected"] += 1
    app["stats"][f"{role}_rejected:{reason}"] += 1
    fields = {"reason": reason}
    if key:
        fields = {"room": room_tag(app, key), **fields}
    event(app, f"{role}_rejected", **fields)


def summary(app, name="summary"):
    stats = app["stats"]
    counts = " ".join(f"{key}={stats[key]}" for key in sorted(stats))
    log.info("%s uptime=%s %s rooms=%d%s", name, duration(time.monotonic() - app["started"]), online(app),
             len(app["rooms"]), f" {counts}" if counts else "")


async def summaries(app):
    while True:
        await asyncio.sleep(SUMMARY_INTERVAL)
        summary(app)


def valid_text(value, limit):
    if not isinstance(value, str) or not value:
        return False
    try:
        return len(value.encode('utf-8')) <= limit
    except UnicodeError:
        return False


def validate_signal(value, role):
    if not isinstance(value, dict):
        return False
    kind = value.get("type")
    if kind == "offer":
        return role == "viewer" and valid_text(value.get("sdp"), MAX_SDP)
    if kind == "answer":
        return role == "camera" and valid_text(value.get("sdp"), MAX_SDP)
    if kind == "candidate":
        return valid_text(value.get("candidate"), MAX_SHORT)
    if kind == "customized":
        return valid_text(value.get("data"), MAX_SHORT)
    return kind == "bye"


async def error_close(ws, message):
    await ws.send_json({"type": "error", "message": message})
    await ws.close(code=WSCloseCode.POLICY_VIOLATION)


async def safe_send(ws, value):
    if not ws.closed:
        try:
            await ws.send_json(value)
        except ConnectionError:
            pass


async def handle(request, role):
    app = request.app
    stats = app["stats"]
    stats[f"{role}_attempts"] += 1
    if app["active"] >= ROOM_LIMIT * 2:
        reject(app, role, "overloaded")
        raise web.HTTPServiceUnavailable()
    app["active"] += 1
    ws = web.WebSocketResponse(heartbeat=20, max_msg_size=MAX_MESSAGE)
    rooms = app["rooms"]
    room = None
    joined_at = 0.0
    reason = "closed"
    try:
        try:
            await ws.prepare(request)
        except web.HTTPException:
            # Plain HTTP requests (scanners, link previews): counted in the
            # summary only, so they cannot flood the log.
            stats[f"{role}_rejected"] += 1
            stats[f"{role}_rejected:not_websocket"] += 1
            raise
        try:
            first = await asyncio.wait_for(ws.receive(), timeout=JOIN_TIMEOUT)
        except asyncio.TimeoutError:
            reject(app, role, "join_timeout")
            await ws.close(code=WSCloseCode.POLICY_VIOLATION)
            return ws
        if first.type != WSMsgType.TEXT:
            reject(app, role, "left_before_join")
            return ws
        try:
            join = json.loads(first.data)
        except json.JSONDecodeError:
            reject(app, role, "bad_join")
            return ws
        if not isinstance(join, dict) or join.get("type") != "join":
            reject(app, role, "bad_join")
            await error_close(ws, "Join a room before sending signals")
            return ws
        key = join.get("room")
        if not isinstance(key, str) or not ROOM_RE.fullmatch(key):
            reject(app, role, "invalid_room")
            await error_close(ws, "Invalid room membership")
            return ws
        if role == "camera":
            secret = join.get("secret")
            if not isinstance(secret, str) or not secret.isascii() or not hmac.compare_digest(secret, app["secret"]):
                reject(app, role, "bad_secret")
                await error_close(ws, "Invalid API secret")
                return ws
        pair = rooms.get(key)
        if role == "viewer" and (pair is None or pair.get("camera") is None):
            # Wrong MAC/password, or the camera is offline.
            reject(app, role, "camera_not_found")
            await error_close(ws, "Camera not found; check MAC/password and camera connection")
            return ws
        if pair is not None and pair.get(role) is not None:
            reject(app, role, "already_connected", key)
            await error_close(ws, "Camera or viewer is already connected")
            return ws
        if pair is None and len(rooms) >= ROOM_LIMIT:
            reject(app, role, "server_full")
            await error_close(ws, "Signaling server is full")
            return ws
        if pair is None:
            pair = {"camera": None, "viewer": None}
            rooms[key] = pair
        pair[role] = ws
        pair[f"{role}_preserves_peer"] = join.get("preserve_peer") is True
        room = key
        joined_at = time.monotonic()
        stats[f"{role}_joined"] += 1
        event(app, f"{role}_connected", room=room_tag(app, key))
        await ws.send_json({"type": "joined"})
        if role == "viewer" and pair.get("camera") is not None:
            await safe_send(pair["camera"], {"type": "customized", "data": "connect"})
        async for message in ws:
            if message.type != WSMsgType.TEXT:
                reason = "binary_message" if message.type == WSMsgType.BINARY else "connection_error"
                break
            try:
                value = json.loads(message.data)
            except json.JSONDecodeError:
                reason = "protocol_violation"
                break
            if not validate_signal(value, role):
                reason = "protocol_violation"
                break
            other = pair["viewer"] if role == "camera" else pair["camera"]
            if other is None:
                if value.get("type") == "bye":
                    continue
                reason = "peer_gone"
                break
            await safe_send(other, value)
    except asyncio.CancelledError:
        # aiohttp cancels the handler when the client's connection drops.
        if room:
            reason = "connection_lost"
        else:
            reject(app, role, "left_before_join")
        raise
    finally:
        app["active"] -= 1
        if room:
            pair = rooms.get(room)
            if pair is not None:
                pair[role] = None
                other_role = "viewer" if role == "camera" else "camera"
                other = pair[other_role]
                if other is not None:
                    # Older clients use WebSocket closure to disconnect. Only
                    # separate socket loss from bye when both peers support it.
                    preserve = pair.get(f"{role}_preserves_peer") and pair.get(f"{other_role}_preserves_peer")
                    kind = "signaling_lost" if preserve else "bye"
                    await safe_send(other, {"type": kind})
                if pair["camera"] is None and pair["viewer"] is None:
                    rooms.pop(room, None)
            stats[f"{role}_disconnected"] += 1
            event(app, f"{role}_disconnected", room=room_tag(app, room), reason=reason,
                  duration=duration(time.monotonic() - joined_at))
    return ws


def main():
    secret = Path("/run/secrets/signaling_api_secret").read_text().strip()
    if not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", secret):
        sys.exit("SIGNALING_API_SECRET must contain 32-128 URL-safe characters")
    setup_logging()
    app = web.Application()
    app["secret"] = secret
    app["rooms"] = {}
    app["active"] = 0
    app["stats"] = collections.Counter()
    app["started"] = time.monotonic()
    app["tag_salt"] = os.urandom(16)

    async def start_summaries(app):
        log.info("started summary_every=%ds", SUMMARY_INTERVAL)
        app["summaries"] = asyncio.create_task(summaries(app))

    async def stop_summaries(app):
        app["summaries"].cancel()
        summary(app, "stopping")
    app.on_startup.append(start_summaries)
    app.on_shutdown.append(stop_summaries)
    app.router.add_get("/camera", lambda request: handle(request, "camera"))
    app.router.add_get("/viewer", lambda request: handle(request, "viewer"))
    web.run_app(app, host="0.0.0.0", port=8080, access_log=None, print=None)


if __name__ == "__main__":
    main()
