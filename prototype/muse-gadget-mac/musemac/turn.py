"""Typed turns: one message to Muse, its reply as the watch's "@chat" frames.

The watch's firmware (``muse_hatch_text_turn``) posts typed text to the main
chat and prints the reply as it streams in; ``prototype/live-voice/muse-serial.js``
reads it. This does the same on the Mac's own Muse session, with the SDK's
``MuseTurn`` deciding which chat events answer the message and when Muse is
done. Each frame is a dict like the JSON after ``@chat``:

    {"seq":N,"type":"ack","bytes":N}                 the message is in
    {"seq":N,"type":"sent","bytes":N}                Muse took it
    {"seq":N,"type":"busy","on":true|false}          Muse is working / stopped
    {"seq":N,"type":"text","msg":I,"text":"..."}     a piece of reply message I
    {"seq":N,"type":"final","msg":I,"text":"..."}    message I's whole text, when the pieces differ
    {"seq":N,"type":"message_done","msg":I,"bytes":N}
    {"seq":N,"type":"done","messages":N,"complete":true|false}
    {"seq":N,"type":"error","text":"WHY"}            the turn ended without an answer
"""

from __future__ import annotations

import asyncio
import itertools
import logging
import time
from typing import Callable

from musegadget.muse_turn import MuseTurn, Reply

log = logging.getLogger(__name__)

SUBSCRIBE_TIMEOUT_S = 20.0
REPLY_TIMEOUT_S = 5 * 60.0     # posted -> first reply; the firmware's typed-turn limits
TURN_CAP_S = 15 * 60.0
POLL_S = 0.25

Emit = Callable[[dict], None]


class TurnFailed(Exception):
    """The turn ended without an answer; the message is the error frame's text."""


class Frames:
    """Numbers frames like the firmware's console: one sequence for the whole bridge."""

    def __init__(self) -> None:
        self._seq = itertools.count(1)

    def make(self, type_: str, text: str | None = None, **fields) -> dict:
        frame = {"seq": next(self._seq), "type": type_, **fields}
        if text is not None:
            frame["text"] = text
        return frame


class ReplyFrames:
    """Turns MuseTurn's replies into text / final / message_done frames, and busy changes."""

    def __init__(self, frames: Frames, emit: Emit) -> None:
        self._frames = frames
        self._emit = emit
        self._index: dict[str, int] = {}
        self._streamed: dict[str, int] = {}
        self._done: set[str] = set()
        self._busy = False

    @property
    def messages(self) -> int:
        return len(self._index)

    def feed(self, events) -> None:
        for event in events:
            if isinstance(event, Reply):
                self._reply(event)

    def busy(self, on: bool) -> None:
        if on != self._busy:
            self._busy = on
            self._emit(self._frames.make("busy", on=on))

    def _reply(self, reply: Reply) -> None:
        mid = reply.message_id
        if mid in self._done:
            return
        i = self._index.setdefault(mid, len(self._index))
        piece = reply.streamed[self._streamed.get(mid, 0):]
        if piece:
            self._streamed[mid] = len(reply.streamed)
            self._emit(self._frames.make("text", piece, msg=i))
        if reply.done:
            self._done.add(mid)
            text = reply.streamed
            if reply.text and reply.text != reply.streamed:
                text = reply.text
                self._emit(self._frames.make("final", text, msg=i))
            self._emit(self._frames.make("message_done", msg=i, bytes=len(text.encode("utf-8"))))


_CLOSED = object()


class ChatHub:
    """One chat subscription per Muse session, handed to whichever turn is listening.

    Like the firmware, it subscribes once and keeps the stream open, so a turn
    never sees a fresh subscription's replay of earlier messages.
    """

    def __init__(self, session) -> None:
        self.session = session
        self._task: asyncio.Task | None = None
        self._listener: asyncio.Queue | None = None

    async def ready(self, timeout: float = SUBSCRIBE_TIMEOUT_S) -> None:
        if self._task is None or self._task.done():
            self.session.chat_subscribed.clear()
            self._task = asyncio.ensure_future(self._pump())
        waiter = asyncio.ensure_future(self.session.chat_subscribed.wait())
        done, _ = await asyncio.wait({waiter, self._task}, timeout=timeout,
                                     return_when=asyncio.FIRST_COMPLETED)
        waiter.cancel()
        if waiter not in done:
            raise TurnFailed("CAN'T SUBSCRIBE TO THE CHAT")

    def listen(self, queue: asyncio.Queue) -> None:
        self._listener = queue

    def unlisten(self, queue: asyncio.Queue) -> None:
        """Stops sending to ``queue``, unless a newer turn already listens instead."""
        if self._listener is queue:
            self._listener = None

    @property
    def closed(self) -> bool:
        return self._task is None or self._task.done()

    async def _pump(self) -> None:
        try:
            async for event in self.session.subscribe_chat():
                if self._listener is not None:
                    self._listener.put_nowait(event)
        except Exception as exc:
            log.warning("chat subscription ended: %s: %s", type(exc).__name__, exc)
        finally:
            if self._listener is not None:
                self._listener.put_nowait(_CLOSED)

    def close(self) -> None:
        if self._task is not None:
            self._task.cancel()


async def run_turn(hub: ChatHub, text: str, frames: Frames, emit: Emit) -> None:
    """Sends ``text`` and emits the reply's frames, ending with done; raises TurnFailed."""
    await hub.ready()
    events: asyncio.Queue = asyncio.Queue()
    hub.listen(events)
    try:
        if hub.closed:
            raise TurnFailed("CONNECTION LOST")
        turn = MuseTurn()
        response = await hub.session.send_chat(text)
        if not response.get("ok"):
            raise TurnFailed(f"MUSE REFUSED THE MESSAGE (HTTP {response.get('status')})")
        replies = ReplyFrames(frames, emit)
        try:
            held = turn.acknowledge(response.get("response") or {})
        except ValueError as exc:
            raise TurnFailed(f"BAD ACK: {exc}") from None
        emit(frames.make("sent", bytes=len(text.encode("utf-8"))))
        replies.feed(held)
        started = time.monotonic()
        while True:
            try:
                event = await asyncio.wait_for(events.get(), POLL_S)
            except asyncio.TimeoutError:
                event = None
            if event is _CLOSED:
                raise TurnFailed("CONNECTION LOST")
            if event is not None:
                try:
                    replies.feed(turn.feed(event))
                except ValueError as exc:
                    raise TurnFailed(str(exc).upper()) from None
            replies.busy(turn.busy)
            now = time.monotonic()
            if (turn.replies and turn.complete(now)) or turn.settled(now):
                emit(frames.make("done", messages=replies.messages, complete=True))
                return
            if not turn.replies and now - started > REPLY_TIMEOUT_S:
                raise TurnFailed("NO REPLY")
            if now - started > TURN_CAP_S:
                emit(frames.make("done", messages=replies.messages, complete=False))
                return
    finally:
        hub.unlisten(events)
