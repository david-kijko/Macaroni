#!/usr/bin/env python3
"""Temporary guest-only HTTPS ingress behind a TLS edge and a pinned SSH forward."""

import argparse
import asyncio
import ipaddress
import json
import re
from pathlib import Path
from urllib.parse import urlsplit

from aiohttp import (
    ClientError,
    ClientSession,
    ClientTimeout,
    DummyCookieJar,
    Fingerprint,
    WSMsgType,
    WSServerHandshakeError,
    web,
)

PLAYER = "/api/share/player/"
READ = {"info", "presence", "stream-state"}
WRITE = {"claim", "chat", "control", "join", "leave", "depart"}
MEDIA_PORTS = range(48552, 48555)  # native slots 2–4; 0 and 1 are owner slots
STATIC = re.compile(
    r"/(assets|css|js|locales)/(?:[a-zA-Z0-9_-][a-zA-Z0-9_.-]*/)*[a-zA-Z0-9_-][a-zA-Z0-9_.-]*"
)
GUEST_PAGE = re.compile(r"/p(?:/[a-zA-Z0-9_-]{43})?/?")
SOCKET = re.compile(r"/ws([234])(?:/stream)?")


async def copy_ws(src, dst):
    async for item in src:
        if item.type == WSMsgType.TEXT:
            await dst.send_str(item.data)
        elif item.type == WSMsgType.BINARY:
            await dst.send_bytes(item.data)
        else:
            break


async def pump(reader, writer):
    while data := await reader.read(65536):
        writer.write(data)
        await writer.drain()


async def duplex(a, b):
    tasks = [asyncio.create_task(a), asyncio.create_task(b)]
    try:
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)


def make_app(origin, upstream, pin, media_ip):
    public = urlsplit(origin)
    if (
        public.scheme != "https"
        or not public.netloc
        or public.path not in ("", "/")
        or public.username
        or public.query
        or public.fragment
    ):
        raise ValueError("Set an HTTPS guest origin.")
    origin = "https://" + public.netloc
    if urlsplit(upstream).hostname not in ("127.0.0.1", "::1"):
        raise ValueError("Upstream must be the local SSH forward.")
    allowed_media = {}  # successful guest stream socket upgrades by port
    media_clients = {port: set() for port in MEDIA_PORTS}

    @web.middleware
    async def upstream_errors(request, handler):
        try:
            return await handler(request)
        except WSServerHandshakeError as exc:
            if exc.status in (401, 403, 404):
                raise web.HTTPForbidden() from None
            raise web.HTTPBadGateway() from None
        except (ClientError, TimeoutError):
            raise web.HTTPBadGateway() from None

    app = web.Application(client_max_size=16384, middlewares=[upstream_errors])
    client = None

    async def media(reader, writer):
        port = writer.get_extra_info("sockname")[1]
        upstream_writer = None
        try:
            if allowed_media.get(port, 0):
                media_clients[port].add(writer)
                remote, upstream_writer = await asyncio.wait_for(
                    asyncio.open_connection("127.0.0.1", port), 5
                )
                await duplex(pump(reader, upstream_writer), pump(remote, writer))
        except (ConnectionError, TimeoutError, OSError):
            pass
        finally:
            media_clients[port].discard(writer)
            writer.close()
            if upstream_writer:
                upstream_writer.close()

    async def lifecycle(_):
        nonlocal client
        servers = []
        async with ClientSession(
            cookie_jar=DummyCookieJar(), timeout=ClientTimeout(total=45)
        ) as client:
            try:
                if media_ip:
                    for port in MEDIA_PORTS:
                        servers.append(
                            await asyncio.start_server(
                                media, str(media_ip), port, limit=65536
                            )
                        )
                yield
            finally:
                for server in servers:
                    server.close()
                await asyncio.gather(*(server.wait_closed() for server in servers))

    async def handle(req):
        if req.host != public.netloc:
            raise web.HTTPNotFound()
        path = req.raw_path.split("?", 1)[0]
        method = req.method
        if req.headers.get("Origin") not in (None, origin):
            raise web.HTTPForbidden()
        if method == "POST" and req.headers.get("Origin") != origin:
            raise web.HTTPForbidden()
        if method in ("GET", "HEAD") and path == "/":
            return web.Response(
                text='<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Macaroni</title><link rel="stylesheet" href="/css/macaroni.css"><main class="guest-welcome"><img src="/assets/logo.png" width="48" height="48" alt=""><h1>Macaroni</h1><p>Open the invitation from the Mac owner to join.</p></main></html>',
                content_type="text/html",
                headers={"Cache-Control": "no-store"},
            )
        if method == "GET" and path == "/api/admin/token":
            # The shared frontend checks this before all POSTs. Public guests have no admin key.
            return web.json_response(
                {"token": ""}, headers={"Cache-Control": "no-store"}
            )
        if method == "GET" and path == "/js/api/MacvmForward.js" and media_ip:
            # Keep browser candidate rewriting on our own guest origin; only guest media ports can be rewritten.
            js = """export function isMacvmForward(){return false;}
export function forwardedCandidate(candidate){
  const f=candidate.split(' ');
  if(f[2]?.toLowerCase()==='tcp' && f.includes('passive') && [48552,48553,48554].includes(Number(f[5]))) f[4]=PUBLIC_MEDIA_ADDRESS;
  return f.join(' ');
}""".replace("PUBLIC_MEDIA_ADDRESS", json.dumps(str(media_ip)))
            return web.Response(
                text=js,
                content_type="text/javascript",
                headers={"Cache-Control": "no-store"},
            )
        sock = SOCKET.fullmatch(path)
        is_socket = (
            sock
            and method == "GET"
            and req.headers.get("Upgrade", "").lower() == "websocket"
        )
        api = path[len(PLAYER) :] if path.startswith(PLAYER) else ""
        static = (
            STATIC.fullmatch(path)
            or GUEST_PAGE.fullmatch(path)
            or path == "/manifest.webmanifest"
        )
        if not (
            is_socket
            or (method in ("GET", "HEAD") and (static or api in READ))
            or (method == "POST" and api in WRITE)
        ):
            raise web.HTTPNotFound()
        # Build headers rather than forwarding arbitrary cookies, authentication, or proxy metadata.
        headers = {"Host": public.netloc, "Accept-Encoding": "identity"}
        if req.headers.get("Origin"):
            headers["Origin"] = origin
        if req.headers.get("User-Agent"):
            headers["User-Agent"] = req.headers["User-Agent"][:512]
        cookie = req.cookies.get("mw_player", "")
        if re.fullmatch(r"[A-Za-z0-9_-]{43}", cookie):
            headers["Cookie"] = "mw_player=" + cookie
        if is_socket:
            if "Cookie" not in headers or req.headers.get("Origin") != origin:
                raise web.HTTPForbidden()
            options = [
                p.strip()
                for p in req.headers.get("Sec-WebSocket-Protocol", "").split(",")
                if p.strip()
            ]
            async with client.ws_connect(
                upstream + path,
                ssl=pin,
                headers=headers,
                protocols=options,
                max_msg_size=8 << 20,
            ) as remote:
                ws = web.WebSocketResponse(
                    protocols=[remote.protocol] if remote.protocol else (),
                    max_msg_size=8 << 20,
                )
                await ws.prepare(req)
                port = 48550 + int(sock.group(1))
                allowed_media[port] = allowed_media.get(port, 0) + 1
                try:
                    await duplex(copy_ws(ws, remote), copy_ws(remote, ws))
                finally:
                    allowed_media[port] -= 1
                    if not allowed_media[port]:
                        for writer in list(media_clients[port]):
                            writer.close()
                    await ws.close()
                return ws
        data = None
        if method == "POST":
            if req.content_type != "application/json":
                raise web.HTTPUnsupportedMediaType()
            data = await req.read()
            if api == "join" and media_ip:
                try:
                    payload = json.loads(data)
                    if not isinstance(payload, dict):
                        raise TypeError()
                    payload["udp_blocked"] = True
                    data = json.dumps(payload).encode()
                except (ValueError, TypeError, UnicodeError):
                    raise web.HTTPBadRequest()
            headers["Content-Type"] = "application/json"
        async with client.request(
            method,
            upstream + path,
            params=req.query,
            headers=headers,
            data=data,
            ssl=pin,
            allow_redirects=False,
        ) as resp:
            outgoing = {
                key: resp.headers[key]
                for key in (
                    "Content-Type",
                    "Content-Security-Policy",
                    "X-Content-Type-Options",
                )
                if key in resp.headers
            }
            outgoing.update(
                {"Referrer-Policy": "no-referrer", "Cache-Control": "private, no-store"}
            )
            if "Content-Security-Policy" in outgoing:
                outgoing["Content-Security-Policy"] = re.sub(
                    r"connect-src [^;]+;",
                    "connect-src 'self' wss://" + public.netloc + ";",
                    outgoing["Content-Security-Policy"],
                )
            if api == "claim" and resp.status == 200 and "mw_player" in resp.cookies:
                outgoing["Set-Cookie"] = resp.cookies["mw_player"].OutputString()
            body = await resp.read()
            if api == "join" and resp.status == 200:
                result = json.loads(body)
                signal = urlsplit(result.get("signalingUrl", "")).path
                if not SOCKET.fullmatch(signal):
                    raise web.HTTPBadGateway()
                result["signalingUrl"] = "wss://" + public.netloc + signal
                body = json.dumps(result).encode()
            return web.Response(status=resp.status, body=body, headers=outgoing)

    app.cleanup_ctx.append(lifecycle)
    app.router.add_route("*", "/{path:.*}", handle)
    return app


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--origin", required=True)
    parser.add_argument(
        "--pin-file",
        type=Path,
        required=True,
        help="SHA256 of the upstream TLS certificate, hex",
    )
    parser.add_argument("--upstream", default="https://127.0.0.1:18443")
    parser.add_argument("--port", type=int, default=18992)
    parser.add_argument(
        "--media-ip",
        type=ipaddress.IPv4Address,
        help="Public interface for the three guest ICE-TCP ports",
    )
    args = parser.parse_args()
    app = make_app(
        args.origin,
        args.upstream,
        Fingerprint(bytes.fromhex(args.pin_file.read_text().strip())),
        args.media_ip,
    )
    web.run_app(app, host="127.0.0.1", port=args.port, access_log=None)
