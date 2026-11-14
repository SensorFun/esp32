"""Viewer-only Janus gateway. Never disclose the upstream API secret to viewers."""
import asyncio
import json
import os
import re
import uuid

from aiohttp import ClientSession, ClientTimeout, WSMsgType, web


async def viewer(request):
    app = request.app
    if app["active"] >= 128:
        raise web.HTTPServiceUnavailable()
    app["active"] += 1
    downstream = web.WebSocketResponse(protocols=["janus-protocol"], heartbeat=20, max_msg_size=32768)
    try:
        await downstream.prepare(request)
        async with app["client"].ws_connect(app["upstream"], protocols=["janus-protocol"], heartbeat=20, max_msg_size=65536) as upstream:
            state = {"stage": "new", "session": None, "handle": None, "joined": False}

            async def responses():
                try:
                    async for event in upstream:
                        if event.type != WSMsgType.TEXT:
                            break
                        value = json.loads(event.data)
                        if value.get("janus") == "success" and state["stage"] in ("creating", "attaching"):
                            identity = value.get("data", {}).get("id")
                            if not isinstance(identity, int) or not 0 < identity < 2**53:
                                raise ValueError("Invalid upstream identity")
                            if state["stage"] == "creating":
                                state.update(session=identity, stage="created")
                            else:
                                state.update(handle=identity, stage="attached")
                        data = value.get("plugindata", {}).get("data", {})
                        if data.get("event") == "joined":
                            state["joined"] = True
                        await downstream.send_json(value)
                        if value.get("janus") in ("error", "timeout", "detached") or data.get("error"):
                            break
                finally:
                    await downstream.close()

            async def commands():
                try:
                    async for event in downstream:
                        if event.type != WSMsgType.TEXT:
                            break
                        value = json.loads(event.data)
                        if not isinstance(value, dict) or not isinstance(value.get("transaction"), str) or len(value["transaction"]) > 128:
                            raise ValueError("Invalid request")
                        kind = value.get("janus")
                        outgoing = {"janus": kind, "transaction": value["transaction"], "apisecret": app["secret"]}
                        # Reconstruct requests; never forward client-selected session IDs,
                        # handles, plugins, roles, credentials, JSEP, or admin operations.
                        if "jsep" in value or "apisecret" in value or "token" in value:
                            raise ValueError("Unsupported credentials or JSEP")
                        if kind == "create" and state["stage"] == "new":
                            state["stage"] = "creating"
                        elif kind == "attach" and state["stage"] == "created" and value.get("plugin") == "janus.plugin.signaling":
                            state["stage"] = "attaching"
                            outgoing["plugin"] = "janus.plugin.signaling"
                        elif kind == "message" and state["handle"]:
                            body = value.get("body")
                            if not isinstance(body, dict):
                                raise ValueError("Invalid request body")
                            if body.get("request") == "join" and not state["joined"] and state["stage"] == "attached":
                                room = body.get("room")
                                if body.get("role") != "viewer" or not isinstance(room, str) or not re.fullmatch(r"[A-F0-9]{32}", room):
                                    raise ValueError("Invalid viewer room")
                                outgoing["body"] = {"request": "join", "room": room, "role": "viewer"}
                                state["stage"] = "joining"
                            elif body.get("request") == "send" and state["joined"] and isinstance(body.get("message"), dict):
                                outgoing["body"] = {"request": "send", "message": body["message"]}
                            else:
                                raise ValueError("Join a viewer room first")
                            outgoing["handle_id"] = state["handle"]
                        elif kind in ("keepalive", "destroy") and state["session"]:
                            pass
                        else:
                            raise ValueError("Unsupported viewer operation")
                        if state["session"]:
                            outgoing["session_id"] = state["session"]
                        await upstream.send_json(outgoing)
                finally:
                    await upstream.close()

            async def handshake_deadline():
                await asyncio.sleep(10)
                if not state["joined"]:
                    await downstream.close()
                    await upstream.close()

            tasks = [asyncio.create_task(responses()), asyncio.create_task(commands()), asyncio.create_task(handshake_deadline())]
            try:
                await asyncio.gather(*tasks[:2])
            except (ValueError, ConnectionError, asyncio.TimeoutError):
                pass
            finally:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
                if state["session"] and not upstream.closed:
                    await upstream.send_json({"janus": "destroy", "session_id": state["session"], "transaction": uuid.uuid4().hex, "apisecret": app["secret"]})
    except (OSError, asyncio.TimeoutError):
        await downstream.close()
    finally:
        app["active"] -= 1
    return downstream


async def camera(request):
    """Camera firmware supplies its own secret; this route never adds one."""
    downstream = web.WebSocketResponse(protocols=["janus-protocol"], heartbeat=20, max_msg_size=32768)
    await downstream.prepare(request)
    try:
        async with request.app["client"].ws_connect(request.app["upstream"], protocols=["janus-protocol"], heartbeat=20) as upstream:
            async def forward(source, target):
                try:
                    async for event in source:
                        if event.type != WSMsgType.TEXT:
                            break
                        await target.send_str(event.data)
                finally:
                    await target.close()
            await asyncio.gather(forward(downstream, upstream), forward(upstream, downstream))
    except (OSError, asyncio.TimeoutError):
        await downstream.close()
    return downstream


async def lifecycle(app):
    async with ClientSession(timeout=ClientTimeout(total=None, sock_connect=5)) as client:
        app["client"] = client
        yield


def main():
    app = web.Application()
    # Set and validated by the launcher; missing settings fail startup.
    app.update(secret=os.environ["JANUS_API_SECRET"], upstream=os.environ["JANUS_UPSTREAM_URL"], active=0)
    app.cleanup_ctx.append(lifecycle)
    app.router.add_get("/viewer", viewer)
    app.router.add_get("/camera", camera)
    web.run_app(app, host="0.0.0.0", port=8080, access_log=None)


if __name__ == "__main__":
    main()
