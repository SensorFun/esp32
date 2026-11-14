"""Two-peer signaling relay. No media, no JSEP: SDP/ICE are opaque payloads
relayed verbatim between exactly one camera and one viewer per room."""
import asyncio
import hmac
import json
import re
import sys
from pathlib import Path

from aiohttp import WSCloseCode, WSMsgType, web

ROOM_RE = re.compile(r"[0-9A-F]{32}")
ROOM_LIMIT = 128
JOIN_TIMEOUT = 10
MAX_MESSAGE = 32768
MAX_SDP = 16384
MAX_SHORT = 8192


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
    if app["active"] >= ROOM_LIMIT * 2:
        raise web.HTTPServiceUnavailable()
    app["active"] += 1
    ws = web.WebSocketResponse(heartbeat=20, max_msg_size=MAX_MESSAGE)
    rooms = app["rooms"]
    room = None
    try:
        await ws.prepare(request)
        try:
            first = await asyncio.wait_for(ws.receive(), timeout=JOIN_TIMEOUT)
        except asyncio.TimeoutError:
            await ws.close(code=WSCloseCode.POLICY_VIOLATION)
            return ws
        if first.type != WSMsgType.TEXT:
            return ws
        try:
            join = json.loads(first.data)
        except json.JSONDecodeError:
            return ws
        if not isinstance(join, dict) or join.get("type") != "join":
            await error_close(ws, "Join a room before sending signals")
            return ws
        key = join.get("room")
        if not isinstance(key, str) or not ROOM_RE.fullmatch(key):
            await error_close(ws, "Invalid room membership")
            return ws
        if role == "camera":
            secret = join.get("secret")
            if not isinstance(secret, str) or not secret.isascii() or not hmac.compare_digest(secret, app["secret"]):
                await error_close(ws, "Invalid API secret")
                return ws
        pair = rooms.get(key)
        if role == "viewer" and (pair is None or pair.get("camera") is None):
            await error_close(ws, "Camera not found; check MAC/password and camera connection")
            return ws
        if pair is not None and pair.get(role) is not None:
            await error_close(ws, "Camera or viewer is already connected")
            return ws
        if pair is None and len(rooms) >= ROOM_LIMIT:
            await error_close(ws, "Signaling server is full")
            return ws
        if pair is None:
            pair = {"camera": None, "viewer": None}
            rooms[key] = pair
        pair[role] = ws
        room = key
        await ws.send_json({"type": "joined"})
        if role == "viewer" and pair.get("camera") is not None:
            await safe_send(pair["camera"], {"type": "customized", "data": "connect"})
        async for message in ws:
            if message.type != WSMsgType.TEXT:
                break
            try:
                value = json.loads(message.data)
            except json.JSONDecodeError:
                break
            if not validate_signal(value, role):
                break
            other = pair["viewer"] if role == "camera" else pair["camera"]
            if other is None:
                if value.get("type") == "bye":
                    continue
                break
            await safe_send(other, value)
    finally:
        app["active"] -= 1
        if room:
            pair = rooms.get(room)
            if pair is not None:
                pair[role] = None
                other = pair["camera"] if pair["camera"] is not None else pair["viewer"]
                if other is not None:
                    await safe_send(other, {"type": "bye"})
                if pair["camera"] is None and pair["viewer"] is None:
                    rooms.pop(room, None)
    return ws


def main():
    secret = Path("/run/secrets/signaling_api_secret").read_text().strip()
    if not re.fullmatch(r"[A-Za-z0-9_-]{32,128}", secret):
        sys.exit("SIGNALING_API_SECRET must contain 32-128 URL-safe characters")
    app = web.Application()
    app["secret"] = secret
    app["rooms"] = {}
    app["active"] = 0
    app.router.add_get("/camera", lambda request: handle(request, "camera"))
    app.router.add_get("/viewer", lambda request: handle(request, "viewer"))
    web.run_app(app, host="0.0.0.0", port=8080, access_log=None)


if __name__ == "__main__":
    main()
