"""The bridge against a fake Muse session: no Bluetooth, no network."""

import asyncio
import json

import pytest

from musemac.bridge import ALLOWED_ORIGIN, Bridge


def event(name, message_id=None, *, parent="u1", **payload):
    data = {"session_id": "main", **payload}
    if message_id is not None:
        data["message_id"] = message_id
    if parent is not None:
        data["reply_to_message_id"] = parent
    return {"type": "event", "event": name, "payload": data}


class FakeSession:
    """A LinkSession's chat surface: subscribe_chat, chat_subscribed, send_chat."""

    def __init__(self, script=(), ack=None):
        self.chat_subscribed = asyncio.Event()
        self.sent = []
        self.events = asyncio.Queue()
        self.subscriptions = 0
        self._script = list(script)
        self._ack = ack or {"ok": True, "status": 200,
                            "response": {"result": {"message_id": "u1", "session_id": "main"}}}

    async def subscribe_chat(self, session_id=None):
        self.subscriptions += 1
        self.chat_subscribed.set()
        while True:
            item = await self.events.get()
            if item is None:
                return
            yield item

    async def send_chat(self, message, session_id=None):
        self.sent.append(message)
        for item in self._script:
            self.events.put_nowait(item)
        return self._ack


ANSWER = [
    event("agent.status", activity_code="thinking"),
    event("delta.text_append", "r1", text="Your dentist "),
    event("delta.text_append", "r1", text="is at 3"),
    event("delta.message_done", "r1", display_text="Your dentist is at 3 pm."),
    event("agent.status", activity_code="idle"),
    event("task.status", status="completed"),
]


async def http(port, method, path, body=None, origin=ALLOWED_ORIGIN, host=None):
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    data = json.dumps(body).encode() if body is not None else b""
    head = [f"{method} {path} HTTP/1.1", f"Host: {host or f'127.0.0.1:{port}'}",
            f"Content-Length: {len(data)}", "Content-Type: application/json"]
    if origin:
        head.append(f"Origin: {origin}")
    writer.write(("\r\n".join(head) + "\r\n\r\n").encode() + data)
    await writer.drain()
    raw = await asyncio.wait_for(reader.read(), 10)
    writer.close()
    head, _, rest = raw.partition(b"\r\n\r\n")
    lines = head.decode().split("\r\n")
    headers = dict(line.split(": ", 1) for line in lines[1:])
    return int(lines[0].split()[1]), headers, rest


def frames(body: bytes) -> list[dict]:
    return [json.loads(chunk[len(b"data: "):]) for chunk in body.split(b"\n\n")
            if chunk.startswith(b"data: ")]


def serve(session, scenario):
    async def main():
        bridge = Bridge(session=lambda: session,
                        status=lambda: {"board": "Mac (test)", "chat": True, "device": {}},
                        port=0)
        server = await bridge.start()
        try:
            return await scenario(bridge.port)
        finally:
            server.close()
    return asyncio.run(main())


def test_ask_streams_the_watch_frames_in_order():
    session = FakeSession(ANSWER)
    status, headers, body = serve(session, lambda port: http(port, "POST", "/ask", {"text": "When's my dentist?"}))
    assert status == 200
    assert headers["Content-Type"] == "text/event-stream"
    assert headers["Access-Control-Allow-Origin"] == ALLOWED_ORIGIN
    got = frames(body)
    assert [f["seq"] for f in got] == list(range(1, len(got) + 1))
    assert [{k: v for k, v in f.items() if k != "seq"} for f in got] == [
        {"type": "ack", "bytes": 18},
        {"type": "sent", "bytes": 18},
        {"type": "busy", "on": True},
        {"type": "text", "msg": 0, "text": "Your dentist "},
        {"type": "text", "msg": 0, "text": "is at 3"},
        {"type": "final", "msg": 0, "text": "Your dentist is at 3 pm."},
        {"type": "message_done", "msg": 0, "bytes": 24},
        {"type": "busy", "on": False},
        {"type": "done", "messages": 1, "complete": True},
    ]
    assert session.sent == ["When's my dentist?"]


def test_final_is_left_out_when_the_pieces_add_up():
    session = FakeSession([
        event("delta.text_append", "r1", text="Hi."),
        event("delta.message_done", "r1", display_text="Hi."),
        event("task.status", status="completed"),
    ])
    _, _, body = serve(session, lambda port: http(port, "POST", "/ask", {"text": "hello"}))
    assert [f["type"] for f in frames(body)] == ["ack", "sent", "text", "message_done", "done"]


def test_events_for_other_requests_are_not_part_of_the_reply():
    session = FakeSession([
        event("delta.text_append", "x1", parent="someone-else", text="Not yours"),
        event("delta.text_append", "r1", text="Yours"),
        event("delta.message_done", "r1", display_text="Yours"),
        event("task.status", status="completed"),
    ])
    _, _, body = serve(session, lambda port: http(port, "POST", "/ask", {"text": "hello"}))
    assert [f.get("text") for f in frames(body) if f["type"] == "text"] == ["Yours"]


def test_muse_refusing_the_message_is_an_error_frame():
    session = FakeSession(ack={"ok": False, "status": 503, "response": None})
    _, _, body = serve(session, lambda port: http(port, "POST", "/ask", {"text": "hello"}))
    assert [(f["type"], f.get("text")) for f in frames(body)] == [
        ("ack", None), ("error", "MUSE REFUSED THE MESSAGE (HTTP 503)")]


def test_without_a_session_the_ask_fails_like_an_unconfigured_watch():
    _, _, body = serve(None, lambda port: http(port, "POST", "/ask", {"text": "hello"}))
    assert [(f["type"], f.get("text")) for f in frames(body)] == [
        ("ack", None), ("error", "MUSE NOT CONNECTED")]


def test_a_lost_subscription_ends_the_turn():
    session = FakeSession([None])
    _, _, body = serve(session, lambda port: http(port, "POST", "/ask", {"text": "hello"}))
    assert frames(body)[-1]["type"] == "error"
    assert frames(body)[-1]["text"] == "CONNECTION LOST"


def test_a_new_ask_interrupts_the_running_one_and_reuses_the_subscription():
    session = FakeSession()   # the first turn gets no answer

    async def scenario(port):
        first = asyncio.ensure_future(http(port, "POST", "/ask", {"text": "one"}))
        while not session.sent:
            await asyncio.sleep(0.01)
        session._script = ANSWER
        second = await http(port, "POST", "/ask", {"text": "two"})
        return await first, second

    (_, _, first), (_, _, second) = serve(session, scenario)
    assert frames(first)[-1]["type"] == "error" and frames(first)[-1]["text"] == "INTERRUPTED"
    assert frames(second)[-1]["type"] == "done"
    assert session.subscriptions == 1


def test_cancel_ends_the_running_turn():
    session = FakeSession()

    async def scenario(port):
        ask = asyncio.ensure_future(http(port, "POST", "/ask", {"text": "one"}))
        while not session.sent:
            await asyncio.sleep(0.01)
        cancel = await http(port, "POST", "/cancel")
        return await ask, cancel

    (_, _, ask), (status, _, cancel) = serve(session, scenario)
    assert status == 200 and json.loads(cancel) == {"cancelled": True}
    assert frames(ask)[-1]["text"] == "CANCELLED"


def test_status_mirrors_the_watch():
    status, headers, body = serve(FakeSession(), lambda port: http(port, "GET", "/status"))
    assert status == 200
    assert json.loads(body) == {"board": "Mac (test)", "chat": True, "device": {}}


def test_requests_without_an_origin_are_allowed():
    status, headers, _ = serve(FakeSession(), lambda port: http(port, "GET", "/status", origin=None))
    assert status == 200
    assert "Access-Control-Allow-Origin" not in headers


@pytest.mark.parametrize("origin", ["http://localhost:8000", "https://example.com", "null"])
def test_other_origins_are_refused(origin):
    session = FakeSession(ANSWER)
    status, headers, _ = serve(session, lambda port: http(port, "POST", "/ask", {"text": "hi"}, origin=origin))
    assert status == 403
    assert "Access-Control-Allow-Origin" not in headers
    assert session.sent == []


def test_another_host_name_is_refused():
    status, _, _ = serve(FakeSession(), lambda port: http(port, "GET", "/status", host="evil.example:8766"))
    assert status == 403


def test_preflight_allows_the_page():
    status, headers, _ = serve(FakeSession(), lambda port: http(port, "OPTIONS", "/ask"))
    assert status == 204
    assert headers["Access-Control-Allow-Origin"] == ALLOWED_ORIGIN
    assert headers["Access-Control-Allow-Headers"] == "Content-Type"


@pytest.mark.parametrize("body", [{}, {"text": ""}, {"text": "  "}, {"text": 3}, ["text"]])
def test_ask_needs_text(body):
    status, _, _ = serve(FakeSession(), lambda port: http(port, "POST", "/ask", body))
    assert status == 400


def test_unknown_paths_and_methods():
    assert serve(FakeSession(), lambda port: http(port, "GET", "/nope"))[0] == 404
    assert serve(FakeSession(), lambda port: http(port, "GET", "/ask"))[0] == 405
