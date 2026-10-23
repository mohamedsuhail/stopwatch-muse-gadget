"""The local bridge: the watch's typed-turn protocol over HTTP on 127.0.0.1:8766.

    POST /ask     {"text": "..."}  -> text/event-stream, one "@chat" frame per event:
                  data: {"seq":1,"type":"ack","bytes":12}
                  data: {"seq":2,"type":"sent","bytes":12}
                  ...
                  data: {"seq":9,"type":"done","messages":1,"complete":true}
                  The stream ends after done or error. Closing it cancels the turn.
    POST /cancel  ends the running turn (its stream gets error "CANCELLED")
    GET  /status  {"board", "chat", "device": {...}}, like the watch's ">status"

One turn at a time: a new /ask ends the running one with error "INTERRUPTED",
as a new ">chat=" does on the watch. Browsers may call it only from the
live-voice page's origin; requests from any other origin, or naming another
Host (DNS rebinding), get 403.
"""

from __future__ import annotations

import asyncio
import json
import logging
import socket
from typing import Callable

from musemac.turn import ChatHub, Frames, TurnFailed, run_turn

log = logging.getLogger(__name__)

HOST = "127.0.0.1"
PORT = 8766
ALLOWED_ORIGIN = "http://localhost:8765"
MAX_BODY = 64 * 1024
MAX_TEXT_BYTES = 32 * 1024
HEADER_TIMEOUT_S = 10

_REASONS = {200: "OK", 204: "No Content", 400: "Bad Request", 403: "Forbidden",
            404: "Not Found", 405: "Method Not Allowed", 413: "Payload Too Large"}


class Request:
    def __init__(self, method: str, path: str, headers: dict[str, str], body: bytes) -> None:
        self.method, self.path, self.headers, self.body = method, path, headers, body


async def read_request(reader: asyncio.StreamReader) -> Request | None:
    head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), HEADER_TIMEOUT_S)
    lines = head.decode("latin-1").split("\r\n")
    parts = lines[0].split(" ")
    if len(parts) != 3:
        return None
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, _, value = line.partition(":")
            headers[key.strip().lower()] = value.strip()
    length = int(headers.get("content-length") or 0)
    if length > MAX_BODY:
        raise ValueError("body too large")
    body = await asyncio.wait_for(reader.readexactly(length), HEADER_TIMEOUT_S) if length else b""
    return Request(parts[0], parts[1].split("?", 1)[0], headers, body)


class Bridge:
    """Serves the bridge on top of a Muse session; ``session()`` returns the live one or None."""

    def __init__(self, session: Callable[[], object], status: Callable[[], dict],
                 port: int = PORT) -> None:
        self._session = session
        self._status = status
        self.port = port
        self._frames = Frames()
        self._hub: ChatHub | None = None
        self._turn: asyncio.Task | None = None
        self._turn_ended: Callable[[str], None] | None = None

    async def start(self, host: str = HOST) -> asyncio.AbstractServer:
        server = await asyncio.start_server(self._client, host, self.port, family=socket.AF_INET)
        self.port = server.sockets[0].getsockname()[1]
        log.info("bridge on http://%s:%d (browsers: %s only)", host, self.port, ALLOWED_ORIGIN)
        return server

    # -- HTTP -------------------------------------------------------------------

    async def _client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            try:
                request = await read_request(reader)
            except ValueError:
                await self._respond(writer, 413, {"error": "body too large"})
                return
            except (asyncio.IncompleteReadError, asyncio.TimeoutError, asyncio.LimitOverrunError):
                return
            if request is None:
                await self._respond(writer, 400, {"error": "bad request"})
                return
            await self._route(request, reader, writer)
        except (ConnectionError, OSError):
            pass
        finally:
            writer.close()

    def _cors(self, request: Request) -> dict[str, str]:
        if request.headers.get("origin") == ALLOWED_ORIGIN:
            return {"Access-Control-Allow-Origin": ALLOWED_ORIGIN, "Vary": "Origin"}
        return {}

    def _allowed(self, request: Request) -> bool:
        origin = request.headers.get("origin")
        host = request.headers.get("host", "")
        return (origin in (None, ALLOWED_ORIGIN)
                and host in (f"127.0.0.1:{self.port}", f"localhost:{self.port}"))

    async def _route(self, request: Request, reader, writer) -> None:
        cors = self._cors(request)
        if not self._allowed(request):
            await self._respond(writer, 403, {"error": "forbidden"})
            return
        if request.method == "OPTIONS":
            await self._respond(writer, 204, None, {
                **cors,
                "Access-Control-Allow-Methods": "GET, POST",
                "Access-Control-Allow-Headers": "Content-Type",
                "Access-Control-Max-Age": "600",
            })
            return
        routes = {("GET", "/status"): self._get_status,
                  ("POST", "/ask"): self._post_ask,
                  ("POST", "/cancel"): self._post_cancel}
        handler = routes.get((request.method, request.path))
        if handler is None:
            known = any(path == request.path for _, path in routes)
            await self._respond(writer, 405 if known else 404, {"error": "not found"}, cors)
            return
        await handler(request, reader, writer, cors)

    async def _respond(self, writer, status: int, body: dict | None,
                       headers: dict[str, str] | None = None) -> None:
        data = json.dumps(body).encode() if body is not None else b""
        head = [f"HTTP/1.1 {status} {_REASONS.get(status, 'Error')}",
                "Connection: close", f"Content-Length: {len(data)}"]
        if body is not None:
            head.append("Content-Type: application/json")
        head += [f"{k}: {v}" for k, v in (headers or {}).items()]
        writer.write(("\r\n".join(head) + "\r\n\r\n").encode() + data)
        await writer.drain()

    # -- Endpoints --------------------------------------------------------------

    async def _get_status(self, request, reader, writer, cors) -> None:
        await self._respond(writer, 200, self._status(), cors)

    async def _post_cancel(self, request, reader, writer, cors) -> None:
        cancelled = self._end_turn("CANCELLED")
        await self._respond(writer, 200, {"cancelled": cancelled}, cors)

    async def _post_ask(self, request, reader, writer, cors) -> None:
        try:
            text = json.loads(request.body).get("text")
        except (ValueError, AttributeError):
            text = None
        if not isinstance(text, str) or not text.replace("\0", "").strip():
            await self._respond(writer, 400, {"error": 'expected {"text": "..."}'}, cors)
            return
        text = text.replace("\0", "")
        if len(text.encode("utf-8")) > MAX_TEXT_BYTES:
            await self._respond(writer, 413, {"error": "text too long"}, cors)
            return

        writer.write(("\r\n".join([
            "HTTP/1.1 200 OK", "Content-Type: text/event-stream", "Cache-Control: no-cache",
            "Connection: close", "X-Accel-Buffering: no", *[f"{k}: {v}" for k, v in cors.items()],
        ]) + "\r\n\r\n").encode())

        def emit(frame: dict) -> None:
            writer.write(b"data: " + json.dumps(frame, ensure_ascii=False).encode() + b"\n\n")

        self._end_turn("INTERRUPTED")
        ended: asyncio.Future = asyncio.get_running_loop().create_future()

        def end(why: str) -> None:
            if not ended.done():
                ended.set_result(why)

        emit(self._frames.make("ack", bytes=len(text.encode("utf-8"))))
        turn = asyncio.ensure_future(self._run(text, emit))
        self._turn, self._turn_ended = turn, end
        hangup = asyncio.ensure_future(reader.read(1))   # the page closing its stream
        try:
            waiting = {turn, hangup, ended}
            while turn in waiting:
                done, waiting = await asyncio.wait(waiting, return_when=asyncio.FIRST_COMPLETED)
                await writer.drain()
                if ended in done:
                    emit(self._frames.make("error", ended.result()))
                    break
                if hangup in done:
                    log.info("the page closed its stream; ending the turn")
                    break
            await writer.drain()
        finally:
            hangup.cancel()
            turn.cancel()
            if self._turn is turn:
                self._turn = self._turn_ended = None

    def _end_turn(self, why: str) -> bool:
        if self._turn is None or self._turn.done():
            return False
        self._turn_ended(why)
        return True

    async def _run(self, text: str, emit) -> None:
        session = self._session()
        if session is None:
            emit(self._frames.make("error", "MUSE NOT CONNECTED"))
            return
        if self._hub is None or self._hub.session is not session:
            if self._hub is not None:
                self._hub.close()
            self._hub = ChatHub(session)
        log.info("typed turn: %d bytes", len(text.encode("utf-8")))
        try:
            await run_turn(self._hub, text, self._frames, emit)
        except TurnFailed as why:
            log.warning("turn failed: %s", why)
            emit(self._frames.make("error", str(why)))
        except (ConnectionError, asyncio.TimeoutError) as exc:
            log.warning("turn failed: %s", type(exc).__name__)
            emit(self._frames.make("error", "CONNECTION LOST"))
