#!/usr/bin/env python3
"""TCP link to the board: the full-featured half of the two links.

This module speaks the frame protocol from the design doc, section 5, and it
is the only place the byte format is written down on the host side --
mock_board.py imports the codec from here rather than growing a second copy,
because two independently maintained encoders is exactly how a protocol ends
up with two dialects.

The framing mirrors app/web_tool/wt_protocol.c on purpose, including the
failure behaviour: a length over the limit or an unknown type is a broken
peer, and the answer is to drop the connection rather than try to resync.
Resyncing means guessing where the next header starts, and a wrong guess
produces frames assembled out of the middle of a JPEG.
"""

from __future__ import annotations

import asyncio
import json
import struct
import time
from typing import Awaitable, Callable, Optional

HDR_LEN = 8
MAX_PAYLOAD = 64 * 1024
FRAME_META_LEN = 8

TYPE_REQ = 0x01
TYPE_RSP = 0x02
TYPE_EVT_LOG = 0x03
TYPE_EVT_FRAME = 0x04
TYPE_PING = 0x05
TYPE_PONG = 0x06

# Board -> console, first frame after the TLS handshake when the board dialled
# out.  See wt_protocol.h for why a token is needed at all: TLS authenticates
# only this console, so without it anything that can reach the port could
# impersonate a board -- and be handed an API key by the operator.
TYPE_HELLO = 0x07

KNOWN_TYPES = frozenset(
    (TYPE_REQ, TYPE_RSP, TYPE_EVT_LOG, TYPE_EVT_FRAME, TYPE_PING, TYPE_PONG,
     TYPE_HELLO)
)

DEFAULT_PORT = 8888

_HDR = struct.Struct("<BBHI")


class ProtocolError(Exception):
    """The peer is not speaking this protocol.  Not recoverable in place."""


def encode_frame(ftype: int, req_id: int, payload: bytes = b"") -> bytes:
    if ftype not in KNOWN_TYPES:
        raise ProtocolError("unknown frame type 0x%02x" % ftype)
    if len(payload) > MAX_PAYLOAD:
        raise ProtocolError("payload %d exceeds %d" % (len(payload), MAX_PAYLOAD))
    return _HDR.pack(ftype, 0, req_id & 0xFFFF, len(payload)) + payload


def fnv1a(data: bytes) -> int:
    """Same constants as tools/b64frames.py and wt_fnv1a()."""
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def decode_frame_event(payload: bytes) -> tuple[int, int, bytes]:
    """Split an EVT_FRAME payload into (seq, fnv1a, jpeg)."""
    if len(payload) < FRAME_META_LEN:
        raise ProtocolError("EVT_FRAME payload is %d bytes" % len(payload))
    seq, digest = struct.unpack("<II", payload[:FRAME_META_LEN])
    return seq, digest, payload[FRAME_META_LEN:]


class FrameParser:
    """Streaming decoder.  Feed it whatever recv() returned; iterate frames."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes):
        """Yield (ftype, req_id, payload) for every complete frame."""
        self._buf += data
        while True:
            if len(self._buf) < HDR_LEN:
                return
            ftype, _flags, req_id, length = _HDR.unpack_from(self._buf, 0)
            if ftype not in KNOWN_TYPES:
                raise ProtocolError("unknown frame type 0x%02x" % ftype)
            if length > MAX_PAYLOAD:
                raise ProtocolError("frame declares %d bytes, limit is %d"
                                    % (length, MAX_PAYLOAD))
            if len(self._buf) < HDR_LEN + length:
                return
            payload = bytes(self._buf[HDR_LEN:HDR_LEN + length])
            del self._buf[:HDR_LEN + length]
            yield ftype, req_id, payload


class BoardLink:
    """One connection to one board, with reconnection.

    Reconnection is deliberately noisy: every attempt reports its number and
    the reason the last one failed.  A silent infinite retry loop is the same
    experience as a hang, and it hides the difference between "the board is
    rebooting" and "the whitelist rejects us".
    """

    def __init__(self, host: str, port: int = DEFAULT_PORT,
                 on_log: Optional[Callable[[dict], Awaitable[None]]] = None,
                 on_frame: Optional[Callable[[int, int, bytes], Awaitable[None]]] = None,
                 on_state: Optional[Callable[[dict], Awaitable[None]]] = None,
                 connect_timeout: float = 4.0,
                 ping_interval: float = 8.0,
                 ping_timeout: float = 12.0) -> None:
        self.host = host
        self.port = port
        self.on_log = on_log
        self.on_frame = on_frame
        self.on_state = on_state
        self.connect_timeout = connect_timeout
        # Liveness.  A board that reboots or loses Wi-Fi does not send a FIN --
        # it simply stops existing -- so a reader parked on the socket waits for
        # ever and the page keeps showing "connected" while every command times
        # out.  That is exactly what was observed on 2026-08-18.  PING/PONG is
        # what turns "silently dead" into "disconnected", which the page can
        # act on and the operator can believe.
        self.ping_interval = ping_interval
        self.ping_timeout = ping_timeout
        self._keepalive_task: Optional[asyncio.Task] = None
        self._last_rx = 0.0

        self._reader: Optional[asyncio.StreamReader] = None
        self._writer: Optional[asyncio.StreamWriter] = None
        self._next_id = 1
        self._pending: dict[int, asyncio.Future] = {}
        self._reader_task: Optional[asyncio.Task] = None
        self._run = False

        self.connected = False
        self.attempts = 0
        self.last_error: Optional[str] = None
        self.connected_since: Optional[float] = None
        self.hello: Optional[dict] = None
        self._hello_event: Optional[asyncio.Event] = None

    async def wait_hello(self, timeout: float = 5.0) -> Optional[dict]:
        """The board's first frame, or None if it never sent one."""
        if self.hello is not None:
            return self.hello
        self._hello_event = asyncio.Event()
        try:
            await asyncio.wait_for(self._hello_event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            return None
        return self.hello

    # ---- state reporting ------------------------------------------------

    def state(self) -> dict:
        return {
            "connected": self.connected,
            "host": self.host,
            "port": self.port,
            "attempts": self.attempts,
            "last_error": self.last_error,
            "connected_since": self.connected_since,
        }

    async def _announce(self) -> None:
        if self.on_state is not None:
            await self.on_state(self.state())

    # ---- connection ----------------------------------------------------

    async def connect_once(self) -> None:
        """One attempt.  Raises on failure so callers can report why."""
        # A previous reader must not outlive its connection: one leaked task
        # per reconnect attempt adds up over an afternoon of reboots.
        self._cancel_reader()
        fut = asyncio.open_connection(self.host, self.port)
        self._reader, self._writer = await asyncio.wait_for(
            fut, timeout=self.connect_timeout)
        sock = self._writer.get_extra_info("socket")
        if sock is not None:
            import socket as _socket
            sock.setsockopt(_socket.IPPROTO_TCP, _socket.TCP_NODELAY, 1)
        self.connected = True
        self.connected_since = time.time()
        self.last_error = None
        self._last_rx = time.time()
        self._reader_task = asyncio.create_task(self._read_loop())
        self._keepalive_task = asyncio.create_task(self._keepalive_loop())
        await self._announce()

    async def adopt(self, reader: asyncio.StreamReader,
                    writer: asyncio.StreamWriter, origin: str) -> None:
        """Take over a stream pair somebody else accepted.

        Used by BoardServer: the board dials in, so the connection is made at
        the other end, but everything after that -- framing, request matching,
        event dispatch -- is identical and should not be a second
        implementation.
        """
        self._cancel_reader()
        self._reader, self._writer = reader, writer
        self.host = origin
        self.connected = True
        self.connected_since = time.time()
        self.last_error = None
        self._last_rx = time.time()

        # TCP keepalive as a backstop under the application-level PING: it is
        # the only thing that eventually reclaims a socket whose peer vanished
        # without closing, if the process is not otherwise reading.
        sock = writer.get_extra_info("socket")
        if sock is not None:
            import socket as _socket
            try:
                sock.setsockopt(_socket.SOL_SOCKET, _socket.SO_KEEPALIVE, 1)
            except OSError:
                pass

        self._reader_task = asyncio.create_task(self._read_loop())
        self._keepalive_task = asyncio.create_task(self._keepalive_loop())
        await self._announce()

    async def _keepalive_loop(self) -> None:
        """Send PING periodically; drop the link when nothing comes back.

        Any received frame counts as liveness, not just the PONG -- a board
        streaming camera frames is obviously alive, and demanding a PONG on top
        of that would drop a busy link.
        """
        try:
            while self.connected:
                await asyncio.sleep(self.ping_interval)
                if not self.connected:
                    return
                silent = time.time() - self._last_rx
                if silent < self.ping_interval:
                    continue
                if not await self.ping():
                    await self._drop("could not send PING")
                    return
                await asyncio.sleep(self.ping_timeout - self.ping_interval
                                    if self.ping_timeout > self.ping_interval
                                    else 1.0)
                if self.connected and time.time() - self._last_rx \
                        >= self.ping_timeout:
                    await self._drop(
                        "board stopped answering (no frame for %.0fs)"
                        % (time.time() - self._last_rx))
                    return
        except asyncio.CancelledError:
            raise
        except Exception as exc:                       # noqa: BLE001
            await self._drop("keepalive: %s" % exc)

    async def probe(self) -> bool:
        """Is the board reachable?  Used by the bootstrap state machine.

        Deliberately a real connect and a real command rather than a ping: the
        network this runs on blocks ICMP, and a TCP handshake that succeeds
        into a firewall would still not prove web_tool is listening.
        """
        try:
            await self.connect_once()
        except Exception as exc:                      # noqa: BLE001
            self.last_error = "%s: %s" % (type(exc).__name__, exc)
            return False
        try:
            rsp = await self.request("sys.status", timeout=4.0)
            return bool(rsp.get("ok"))
        except Exception as exc:                      # noqa: BLE001
            self.last_error = "%s: %s" % (type(exc).__name__, exc)
            return False

    async def run_forever(self, max_backoff: float = 8.0) -> None:
        """Keep the link up, reporting each attempt."""
        self._run = True
        backoff = 0.5
        while self._run:
            if not self.connected:
                self.attempts += 1
                try:
                    await self.connect_once()
                    backoff = 0.5
                except Exception as exc:              # noqa: BLE001
                    self.last_error = "%s: %s" % (type(exc).__name__, exc)
                    await self._announce()
                    await asyncio.sleep(backoff)
                    backoff = min(backoff * 2, max_backoff)
                    continue
            await asyncio.sleep(0.25)

    def _cancel_reader(self) -> None:
        for attr in ("_reader_task", "_keepalive_task"):
            task = getattr(self, attr, None)
            setattr(self, attr, None)
            if task is None or task.done():
                continue
            try:
                if task is asyncio.current_task():
                    continue    # this task is closing its own connection
            except RuntimeError:
                pass
            task.cancel()

    async def close(self) -> None:
        self._run = False
        await self._drop("closed by host")
        self._cancel_reader()

    async def _drop(self, reason: str) -> None:
        was = self.connected
        self.connected = False
        self.connected_since = None

        # Keep the *first* reason.  Dropping the link closes the socket, which
        # makes the reader report EOF a moment later; letting that overwrite the
        # original cause turns "the board stopped answering" -- the useful
        # diagnosis -- into "the board closed the connection", which is both
        # wrong and the thing an operator would least want to be told.
        if reason and (was or not self.last_error):
            self.last_error = reason
            if was:
                print("console: board link dropped: %s" % reason, flush=True)
        if self._writer is not None:
            try:
                self._writer.close()
            except Exception:                          # noqa: BLE001
                pass
            self._writer = None
        self._reader = None
        for fut in self._pending.values():
            if not fut.done():
                fut.set_exception(ConnectionError(reason))
        self._pending.clear()
        await self._announce()

    # ---- request / response --------------------------------------------

    async def request(self, cmd: str, args: Optional[dict] = None,
                      timeout: float = 10.0) -> dict:
        if self._writer is None:
            raise ConnectionError("not connected")
        req_id = self._next_id
        self._next_id = self._next_id % 0xFFFE + 1
        body = json.dumps({"cmd": cmd, "args": args or {}}).encode()
        fut: asyncio.Future = asyncio.get_running_loop().create_future()
        self._pending[req_id] = fut
        try:
            self._writer.write(encode_frame(TYPE_REQ, req_id, body))
            await self._writer.drain()
            return await asyncio.wait_for(fut, timeout=timeout)
        finally:
            self._pending.pop(req_id, None)

    async def ping(self, timeout: float = 4.0) -> bool:
        if self._writer is None:
            return False
        try:
            self._writer.write(encode_frame(TYPE_PING, 0))
            await self._writer.drain()
            return True
        except Exception:                              # noqa: BLE001
            return False

    # ---- reader --------------------------------------------------------

    async def _read_loop(self) -> None:
        parser = FrameParser()
        reader = self._reader
        assert reader is not None
        try:
            while True:
                chunk = await reader.read(65536)
                if not chunk:
                    await self._drop("board closed the connection")
                    return
                self._last_rx = time.time()
                for ftype, req_id, payload in parser.feed(chunk):
                    await self._dispatch(ftype, req_id, payload)
        except ProtocolError as exc:
            # Framing lost.  Drop the connection: the alternative is guessing
            # where the next header is, and a wrong guess is silent corruption.
            await self._drop("protocol error: %s" % exc)
        except (ConnectionError, asyncio.IncompleteReadError) as exc:
            await self._drop("link error: %s" % exc)
        except asyncio.CancelledError:
            raise
        except Exception as exc:                       # noqa: BLE001
            await self._drop("%s: %s" % (type(exc).__name__, exc))

    async def _dispatch(self, ftype: int, req_id: int, payload: bytes) -> None:
        if ftype == TYPE_RSP:
            fut = self._pending.get(req_id)
            try:
                body = json.loads(payload.decode("utf-8", "replace"))
            except json.JSONDecodeError:
                body = {"ok": False, "err": "board sent invalid JSON",
                        "errno": -22, "errname": "EINVAL"}
            if fut is not None and not fut.done():
                fut.set_result(body)
            elif self.on_log is not None and (
                    "dropped" in body or "exit" in body or "line" in body):
                # An unmatched RSP carrying log-shaped content is a log event
                # the board refused to let the queue drop: the gap notice, and
                # the exit notice that tells the page a command has finished.
                # Both are useless if lost, so they travel in the class that is
                # never dropped.
                await self.on_log(body)
            return

        if ftype == TYPE_EVT_LOG:
            if self.on_log is not None:
                try:
                    await self.on_log(
                        json.loads(payload.decode("utf-8", "replace")))
                except json.JSONDecodeError:
                    pass
            return

        if ftype == TYPE_EVT_FRAME:
            if self.on_frame is not None:
                seq, digest, jpeg = decode_frame_event(payload)
                await self.on_frame(seq, digest, jpeg)
            return

        if ftype == TYPE_HELLO:
            try:
                self.hello = json.loads(payload.decode("utf-8", "replace"))
            except json.JSONDecodeError:
                self.hello = {}
            if self._hello_event is not None:
                self._hello_event.set()
            return

        if ftype == TYPE_PING:
            if self._writer is not None:
                self._writer.write(encode_frame(TYPE_PONG, req_id))
            return

        # PONG and anything else: nothing to do, and not an error.


class BoardServer:
    """Wait for the board to dial in over TLS.

    This is the direction that works.  The board is behind the access point's
    NAT: measured 2026-08-18, the board reaches this machine in 91 ms while
    nothing inbound arrives, so a console that waits to be called is the only
    one that ever gets a connection.

    Authentication is two-sided and asymmetric, matching what each end can
    actually do:

      this console  proves itself with the TLS certificate, which the board
                    pins by SHA-256 (kvdb `web.fp`)
      the board     proves itself with the token in its HELLO frame, because it
                    has no certificate and cannot generate one

    Without the token check, anything that could reach this port could pretend
    to be the board and be handed whatever the operator typed -- including an
    API key.
    """

    def __init__(self, token: str, port: int, ssl_context,
                 bind: str = "0.0.0.0",
                 on_log=None, on_frame=None, on_state=None,
                 hello_timeout: float = 5.0) -> None:
        self.token = token
        self.port = port
        self.bind = bind
        self.ssl_context = ssl_context
        self.on_log = on_log
        self.on_frame = on_frame
        self.on_state = on_state
        self.hello_timeout = hello_timeout

        self.link: Optional[BoardLink] = None
        self.server: Optional[asyncio.AbstractServer] = None
        self.rejected = 0
        self.accepted = 0
        self.last_reject: Optional[str] = None

    def state(self) -> dict:
        base = self.link.state() if self.link is not None else {
            "connected": False, "host": None, "port": self.port,
            "attempts": 0, "last_error": "waiting for the board to dial in",
        }
        base.update({
            "mode": "inbound-tls",
            "accepted": self.accepted,
            "rejected": self.rejected,
            "last_reject": self.last_reject,
        })
        return base

    async def start(self) -> None:
        self.server = await asyncio.start_server(
            self._on_board, self.bind, self.port, ssl=self.ssl_context)

    async def _on_board(self, reader, writer) -> None:
        peer = writer.get_extra_info("peername")
        origin = "%s:%s" % (peer[0], peer[1]) if peer else "?"

        link = BoardLink(origin, self.port, on_log=self.on_log,
                         on_frame=self.on_frame, on_state=self.on_state)
        await link.adopt(reader, writer, origin)

        hello = await link.wait_hello(self.hello_timeout)
        if hello is None or hello.get("token") != self.token:
            self.rejected += 1
            self.last_reject = (
                "%s sent no hello" % origin if hello is None
                else "%s sent the wrong token" % origin)
            print("console: rejected %s (%s)"
                  % (origin, "no hello" if hello is None
                     else "bad token"), flush=True)
            await link.close()
            try:
                writer.close()
            except Exception:                          # noqa: BLE001
                pass
            if self.on_state is not None:
                await self.on_state(self.state())
            return

        # One board at a time, newest wins -- the same rule the board applies
        # to inbound clients, for the same reason: two sessions would mean two
        # log subscriptions and two camera lifetimes.
        if self.link is not None and self.link.connected:
            print("console: displacing the previous board session", flush=True)
            await self.link.close()

        self.accepted += 1
        self.link = link
        print("console: board %s accepted (proto %s)"
              % (origin, hello.get("proto")), flush=True)
        if self.on_state is not None:
            await self.on_state(self.state())

    async def request(self, cmd: str, args=None, timeout: float = 15.0) -> dict:
        if self.link is None or not self.link.connected:
            raise ConnectionError("the board has not dialled in yet")
        return await self.link.request(cmd, args, timeout=timeout)

    async def close(self) -> None:
        if self.link is not None:
            await self.link.close()
        if self.server is not None:
            self.server.close()
