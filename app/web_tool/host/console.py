#!/usr/bin/env python3
"""Backend: HTTP and WebSocket for the browser, frames and NSH for the board.

    browser  <--HTTP/WS-->  console.py  <--TCP frames-->  web_tool on the board
                                        <--NSH text-->    the serial console

The asymmetry between the two board-side links is visible here and is not
abstracted away.  TCP does everything; serial does bootstrap, early log and
rescue.  When only serial is available the page is told so and disables the
cards that would otherwise pretend to work -- a control that looks live and
silently does nothing is worse than one that is greyed out.

Usage:

    ./console.py                        find the board, serve on :8080
    ./console.py --board 10.192.105.127 skip discovery
    ./console.py --mock                 talk to mock_board.py on localhost
    ./console.py --ssid X --psk Y       credentials for the provisioning path

Nothing here parses human-readable output for structured data, with one
deliberate exception: the bootstrap path reads an address out of `ifconfig`,
because before there is a network there is no other source for it.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from aiohttp import WSMsgType, web

import tlsconf
from board_link import DEFAULT_PORT, BoardLink, BoardServer
from bootstrap import Bootstrap, load_cached_ip
from capture import CaptureSession, list_sessions
from serial_console import SerialBusy, SerialConsole, port_holder

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")


def _primary_address() -> str:
    """The address of ours the board should dial.

    Read from the routing table rather than guessed: this machine has several
    addresses and only one of them is reachable from the board's network.
    """
    import socket
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("10.255.255.255", 1))
            return s.getsockname()[0]
    except OSError:
        return ""
TOKEN_PATH = os.path.join(tlsconf.TLS_DIR, "board-token")

# Default port the board dials in on.  Separate from the browser's HTTPS port
# because the two speak different protocols and only one of them should ever be
# reachable from anywhere but this machine.
DEFAULT_BOARD_PORT = 8899


def ensure_token() -> str:
    """The shared secret the board puts in its HELLO frame.

    Generated once and kept, so the value the operator pasted into kvdb stays
    valid across restarts.  128 bits from os.urandom: it only has to be
    unguessable, and it is never used as a key.
    """
    os.makedirs(tlsconf.TLS_DIR, exist_ok=True)
    if os.path.exists(TOKEN_PATH):
        with open(TOKEN_PATH, encoding="utf-8") as fp:
            token = fp.read().strip()
        if token:
            return token
    token = os.urandom(16).hex()
    with open(TOKEN_PATH, "w", encoding="utf-8") as fp:
        fp.write(token + "\n")
    os.chmod(TOKEN_PATH, 0o600)
    return token


class Console:
    def __init__(self, args) -> None:
        self.args = args
        self.link: BoardLink | None = None
        self.server: BoardServer | None = None
        self.token = ensure_token()
        self.cert_path, self.key_path = tlsconf.ensure_cert()
        self.fingerprint = tlsconf.fingerprint(self.cert_path)
        self.clients: set[web.WebSocketResponse] = set()
        self.capture = CaptureSession()
        self.serial: SerialConsole | None = None   # only while explicitly held
        self.degraded_reason = ""
        self.bootstrap_result = None
        self.fps = 0.0
        # `fuser` is a process spawn, and state() is broadcast on every log
        # line and every frame.  Calling it there meant dozens of subprocess
        # spawns a second, each one blocking the event loop -- which showed up
        # as commands intermittently timing out while the board was perfectly
        # healthy.  Cache it, and only pay for it when someone is actually
        # asking about the port.
        self._holder = None
        self._holder_at = 0.0
        self._frame_times: list[float] = []
        self.dropped_total = 0

    # ---- fan-out to browsers -------------------------------------------

    async def broadcast(self, obj: dict) -> None:
        dead = []
        data = json.dumps(obj)
        for ws in self.clients:
            try:
                await ws.send_str(data)
            except Exception:                          # noqa: BLE001
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)

    async def broadcast_bytes(self, payload: bytes) -> None:
        dead = []
        for ws in self.clients:
            try:
                await ws.send_bytes(payload)
            except Exception:                          # noqa: BLE001
                dead.append(ws)
        for ws in dead:
            self.clients.discard(ws)

    # ---- board callbacks ------------------------------------------------

    async def on_log(self, event: dict) -> None:
        if "dropped" in event:
            self.dropped_total += int(event["dropped"])
        self.capture.write_log(event)
        await self.broadcast({"type": "log", "event": event,
                              "dropped_total": self.dropped_total})

    async def on_frame(self, seq: int, digest: int, jpeg: bytes) -> None:
        now = time.monotonic()
        self._frame_times.append(now)
        self._frame_times = [t for t in self._frame_times if now - t <= 5.0]
        if len(self._frame_times) >= 2:
            span = self._frame_times[-1] - self._frame_times[0]
            self.fps = (len(self._frame_times) - 1) / span if span > 0 else 0.0

        result = self.capture.write_frame(seq, digest, jpeg)

        # The 8-byte prefix is the same seq/checksum the board sent, kept so
        # the page can label a frame and count rejects without a second
        # message that could arrive out of order.
        await self.broadcast_bytes(struct.pack("<II", seq, digest) + jpeg)

        if result.get("written"):
            # Do not reconstruct this path in the browser: CaptureSession is
            # the authority that chose the session, rejected/ directory and
            # filename.  It is absolute, so the operator can copy it directly
            # from the web console into a host shell.
            await self.broadcast({"type": "capture.saved",
                                  "seq": seq,
                                  "path": result["path"],
                                  "ok": result.get("ok", False),
                                  "why": result.get("why", "")})

        if result.get("written") and not result.get("ok", True):
            await self.broadcast({"type": "state", "state": self.state()})

    async def on_link_state(self, _state: dict) -> None:
        await self.broadcast({"type": "state", "state": self.state()})

    # ---- state ----------------------------------------------------------

    def board(self):
        """Whichever transport currently owns the board, or None.

        Inbound (the board dialled us, over TLS) is the production path;
        outbound exists for a board on the same subnet and for the mock.
        """
        if self.server is not None:
            return self.server.link if (
                self.server.link is not None
                and self.server.link.connected) else None
        return self.link if (self.link is not None
                             and self.link.connected) else None

    def pairing(self) -> dict:
        """What has to be typed on the board, once, to pair it with us."""
        return {
            "fingerprint": self.fingerprint,
            "token": self.token,
            "board_port": self.args.board_listen_port,
            "host_hint": tlsconf._local_addresses(),
            "commands": [
                "kvdb set web.host <this machine's IP>",
                "kvdb set web.port %d" % self.args.board_listen_port,
                "kvdb set web.fp %s" % self.fingerprint,
                "kvdb set web.token %s" % self.token,
                "web_tool &",
            ],
        }

    def serial_holder(self, max_age: float = 3.0):
        """Who holds the serial port, at most once every max_age seconds."""
        now = time.time()
        if now - self._holder_at > max_age:
            self._holder = port_holder(self.args.serial_port)
            self._holder_at = now
        return self._holder

    def state(self) -> dict:
        if self.server is not None:
            linkstate = self.server.state()
        elif self.link is not None:
            linkstate = self.link.state()
            linkstate["mode"] = "outbound"
        else:
            linkstate = {"connected": False, "host": None, "attempts": 0,
                         "last_error": "no link yet", "mode": "none"}
        return {
            "link": linkstate,
            "tls": {
                "fingerprint": self.fingerprint,
                "browser": True,
                "board": self.server is not None,
            },
            "serial": {
                "held": self.serial is not None,
                "holder": self.serial_holder(),
                "port": self.args.serial_port,
            },
            "fps": round(self.fps, 2),
            "capture": self.capture.status(),
            "dropped_total": self.dropped_total,
            "degraded": self.link is None or not self.link.connected,
            "degraded_reason": self.degraded_reason,
            "sessions": list_sessions()[:12],
            "pairing": self.pairing(),
        }

    # ---- bootstrap ------------------------------------------------------

    async def do_bootstrap(self) -> dict:
        boot = Bootstrap(port=self.args.board_port,
                         serial_port=self.args.serial_port,
                         ssid=self.args.ssid, psk=self.args.psk)
        result = await boot.run()
        self.bootstrap_result = result
        self.degraded_reason = result.degraded_reason
        if result.ok and result.ip:
            await self.attach(result.ip)
        payload = {"type": "bootstrap", "ok": result.ok, "ip": result.ip,
                   "path": result.path, "steps": result.steps,
                   "degraded_reason": result.degraded_reason,
                   "serial_available": result.serial_available}
        await self.broadcast(payload)
        await self.broadcast({"type": "state", "state": self.state()})
        return payload

    async def attach(self, ip: str) -> None:
        if self.link is not None:
            await self.link.close()
        self.link = BoardLink(ip, self.args.board_port, on_log=self.on_log,
                              on_frame=self.on_frame,
                              on_state=self.on_link_state)
        asyncio.create_task(self.link.run_forever())

    # ---- pairing over serial -------------------------------------------

    def pair_over_serial(self, ssid: str, psk: str) -> dict:
        """Do the whole pairing over the serial console, in one go.

        This exists because the alternative was a ritual: read four kvdb lines
        off the page, open a serial terminal, paste them, start web_tool.  Every
        reboot repeated it, because this board's kvdb does not survive a reset
        -- so the ritual was not a one-time setup cost, it was the normal way to
        use the tool.  That is a bad enough experience to be worth automating,
        and the backend already knows how to talk to the serial console.

        The port is opened for the duration and closed before returning, for the
        same reason as everywhere else here: serial_cmd.sh and autoflash.sh both
        need it free.
        """
        host_ip = self.args.pair_host or _primary_address()
        if not host_ip:
            return {"ok": False, "err": "could not work out which address the "
                                        "board should dial; pass --pair-host"}

        steps = [
            ("kvdb set wifi.ssid %s" % ssid, 2.0),
        ]
        if psk:
            steps.append(("kvdb set wifi.psk %s" % psk, 2.0))
        steps += [
            ("kvdb wifi", 4.0),
            # Two renews: the first request after association is measured to
            # fail, and treating that as the answer would report a working
            # network as broken.
            ("renew wlan0", 7.0),
            ("renew wlan0", 7.0),
            ("kvdb set web.host %s" % host_ip, 2.0),
            ("kvdb set web.port %d" % self.args.board_listen_port, 2.0),
            ("kvdb set web.fp %s" % self.fingerprint, 2.0),
            ("kvdb set web.token %s" % self.token, 2.0),
            ("web_tool &", 3.0),
        ]

        transcript = []
        try:
            with SerialConsole(self.args.serial_port) as con:
                con.open_bridge()
                for cmd, wait in steps:
                    out = con.command(cmd, wait)
                    # The passphrase must not end up in a transcript that goes
                    # to the browser and into the log pane.
                    shown = cmd if not cmd.startswith("kvdb set wifi.psk") \
                        else "kvdb set wifi.psk ****"
                    transcript.append("$ %s\n%s" % (shown, out.strip()))
                running, addr, text = con.read_wlan()
                transcript.append(text.strip())
        except SerialBusy as exc:
            return {"ok": False, "err": str(exc), "transcript": ""}
        except OSError as exc:
            return {"ok": False, "err": "%s: %s" % (type(exc).__name__, exc),
                    "transcript": ""}

        return {
            "ok": bool(running),
            "ip": addr,
            "host": host_ip,
            "transcript": "\n".join(transcript),
            "err": "" if running else
                   "板子没拿到地址（%s）；检查 SSID/密码，或稍后重试" % (addr or "-"),
        }

    # ---- serial ---------------------------------------------------------

    def serial_acquire(self) -> dict:
        if self.serial is not None:
            return {"ok": True, "held": True, "note": "already held"}
        try:
            con = SerialConsole(self.args.serial_port)
            con.open()
        except SerialBusy as exc:
            return {"ok": False, "held": False, "err": str(exc)}
        except OSError as exc:
            return {"ok": False, "held": False, "err": str(exc)}
        self.serial = con
        return {"ok": True, "held": True}

    def serial_release(self) -> dict:
        if self.serial is not None:
            self.serial.close()
            self.serial = None
        # Reported back so the page's lamp reflects the port, not our intent:
        # serial_cmd.sh and autoflash.sh both need it actually free.
        # Fresh, not cached: the operator just asked us to let go, and the
        # answer to "is it free now" must not be three seconds stale.
        self._holder_at = 0.0
        return {"ok": True, "held": False, "holder": self.serial_holder(0.0)}

    def serial_run(self, cmds: list) -> dict:
        if self.serial is not None:
            out = self.serial.rescue(cmds)
            return {"ok": True, "output": out, "held": True}
        try:
            with SerialConsole(self.args.serial_port) as con:
                con.open_bridge()
                out = con.rescue(cmds)
            return {"ok": True, "output": out, "held": False}
        except SerialBusy as exc:
            return {"ok": False, "err": str(exc)}
        except OSError as exc:
            return {"ok": False, "err": str(exc)}

    # ---- HTTP handlers ---------------------------------------------------

    async def handle_index(self, _request):
        return web.FileResponse(os.path.join(WEB_DIR, "index.html"))

    async def handle_ws(self, request):
        ws = web.WebSocketResponse(max_msg_size=4 * 1024 * 1024,
                                   heartbeat=20.0)
        await ws.prepare(request)
        self.clients.add(ws)
        await ws.send_str(json.dumps({"type": "state", "state": self.state()}))
        if self.bootstrap_result is not None:
            r = self.bootstrap_result
            await ws.send_str(json.dumps(
                {"type": "bootstrap", "ok": r.ok, "ip": r.ip, "path": r.path,
                 "steps": r.steps, "degraded_reason": r.degraded_reason,
                 "serial_available": r.serial_available}))

        try:
            async for msg in ws:
                if msg.type is not WSMsgType.TEXT:
                    continue
                try:
                    req = json.loads(msg.data)
                except json.JSONDecodeError:
                    continue
                await self._handle_browser(ws, req)
        finally:
            self.clients.discard(ws)
        return ws

    async def _handle_browser(self, ws, req: dict) -> None:
        op = req.get("op")
        rid = req.get("id")

        async def reply(obj: dict) -> None:
            obj["type"] = "rsp"
            obj["id"] = rid
            await ws.send_str(json.dumps(obj))

        if op == "cmd":
            board = self.board()
            if board is None:
                await reply({"ok": False,
                             "err": "the board has not connected yet",
                             "errno": -128, "errname": "ENOTCONN"})
                return
            try:
                body = await board.request(req.get("cmd", ""),
                                               req.get("args") or {},
                                               timeout=req.get("timeout", 15))
            except asyncio.TimeoutError:
                body = {"ok": False, "err": "board did not answer in time",
                        "errno": -110, "errname": "ETIMEDOUT"}
            except ConnectionError as exc:
                body = {"ok": False, "err": str(exc), "errno": -128,
                        "errname": "ENOTCONN"}
            await reply(body)
            return

        if op == "bootstrap":
            await reply(await self.do_bootstrap())
            return

        if op == "attach":
            ip = req.get("ip")
            if not ip:
                await reply({"ok": False, "err": "ip is required"})
                return
            await self.attach(ip)
            await reply({"ok": True})
            return

        if op == "serial":
            action = req.get("action")
            if action == "acquire":
                await reply(self.serial_acquire())
            elif action == "release":
                await reply(self.serial_release())
            elif action == "run":
                await reply(self.serial_run(req.get("cmds") or []))
            else:
                await reply({"ok": False, "err": "unknown serial action"})
            await self.broadcast({"type": "state", "state": self.state()})
            return

        if op == "capture":
            action = req.get("action")
            if action == "start":
                self.capture = CaptureSession()
                await reply({"ok": True, "data": self.capture.start()})
            elif action == "stop":
                await reply({"ok": True, "data": self.capture.stop()})
            else:
                await reply({"ok": False, "err": "unknown capture action"})
            await self.broadcast({"type": "state", "state": self.state()})
            return

        if op == "state":
            await reply({"ok": True, "data": self.state()})
            return

        if op == "pairing":
            await reply({"ok": True, "data": self.pairing()})
            return

        if op == "pair":
            # Blocking serial work: run it off the event loop so the WebSocket
            # keeps answering and the page does not look frozen.
            result = await asyncio.get_running_loop().run_in_executor(
                None, self.pair_over_serial,
                req.get("ssid", ""), req.get("psk", ""))
            await reply({"ok": result.get("ok", False), "data": result,
                         "err": result.get("err", "")})
            await self.broadcast({"type": "state", "state": self.state()})
            return

        await reply({"ok": False, "err": "unknown op %r" % op})

    # ---- run ------------------------------------------------------------

    def build_app(self) -> web.Application:
        app = web.Application()
        app.router.add_get("/", self.handle_index)
        app.router.add_get("/ws", self.handle_ws)
        app.router.add_static("/static/", WEB_DIR, show_index=False)
        return app


async def amain(args) -> None:
    console = Console(args)
    app = console.build_app()
    runner = web.AppRunner(app)
    await runner.setup()

    browser_ssl = None if args.no_tls else tlsconf.browser_context(
        console.cert_path, console.key_path)
    site = web.TCPSite(runner, args.host, args.port, ssl_context=browser_ssl)
    await site.start()

    scheme = "http" if args.no_tls else "https"
    print("console: %s://%s:%d/" % (scheme, args.host or "0.0.0.0", args.port),
          flush=True)
    if args.no_tls:
        print("console: WARNING --no-tls given; the browser link is in the "
              "clear.  Only for debugging the page itself.", flush=True)

    if args.board:
        # Outbound: only works when this machine can reach the board, i.e. same
        # subnet.  Kept for that case and for the mock.
        await console.attach(args.board)
    elif args.no_bootstrap:
        cached = load_cached_ip()
        if cached:
            await console.attach(cached)
    else:
        # Inbound over TLS: the production path, because the board is behind
        # the access point's NAT and cannot be reached from here.
        console.server = BoardServer(
            token=console.token,
            port=args.board_listen_port,
            ssl_context=tlsconf.board_context(console.cert_path,
                                              console.key_path),
            bind=args.board_listen,
            on_log=console.on_log,
            on_frame=console.on_frame,
            on_state=console.on_link_state)
        await console.server.start()
        print("console: waiting for the board on %s:%d (TLS 1.2, "
              "certificate pinned by the board)"
              % (args.board_listen, args.board_listen_port), flush=True)
        print("\nOn the board, once:", flush=True)
        for line in console.pairing()["commands"]:
            print("  " + line, flush=True)
        print("", flush=True)

    while True:
        await asyncio.sleep(3600)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--board", default="",
                    help="board address; skips the bootstrap state machine")
    ap.add_argument("--board-port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--serial-port", default="/dev/ttyUSB0")
    ap.add_argument("--ssid", default="")
    ap.add_argument("--psk", default="")
    ap.add_argument("--no-bootstrap", action="store_true")
    ap.add_argument("--board-listen", default="0.0.0.0",
                    help="interface the board dials in on")
    ap.add_argument("--board-listen-port", type=int,
                    default=DEFAULT_BOARD_PORT)
    ap.add_argument("--no-tls", action="store_true",
                    help="serve the page over plain HTTP; debugging only")
    ap.add_argument("--pair-host", default="",
                    help="address the board should dial; defaults to whichever "
                         "of ours the routing table picks")
    ap.add_argument("--mock", action="store_true",
                    help="talk to mock_board.py on 127.0.0.1")
    args = ap.parse_args()

    if args.mock and not args.board:
        args.board = "127.0.0.1"

    try:
        asyncio.run(amain(args))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
