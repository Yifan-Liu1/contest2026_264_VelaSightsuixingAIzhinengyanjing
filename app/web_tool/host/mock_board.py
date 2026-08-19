#!/usr/bin/env python3
"""A board that is not a board: speaks the protocol, needs no hardware.

Why this exists: the real board is one device shared by everyone, its serial
port is exclusive, and reflashing it takes a minute.  Testing the backend and
the page against it would mean the tests can only run when nobody else is using
it -- so they would not run.  This answers the same frames over TCP and the
same NSH text over a pty, which is enough to drive every path the backend has.

What it can be told to do, because these are the cases that are hard to
provoke on real hardware:

    --scenario running       already on the network; TCP works at once
    --scenario needs-setup   serial says the interface is on 10.0.0.2, so the
                             backend has to run the provisioning sequence
    --scenario no-network    provisioning runs and still fails: degraded mode
    --refuse                 refuse every TCP connection, as the whitelist does
    --corrupt N              break the checksum of every Nth frame
    --flood N                emit N log lines per second, to force drops

Deliberately imperfect in one way: it answers instantly.  Timing bugs are the
one class this cannot find, and the real-board acceptance list is what covers
them.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import binascii
import json
import os
import pty
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from board_link import (FrameParser, TYPE_EVT_FRAME, TYPE_EVT_LOG, TYPE_HELLO,
                        TYPE_PING, TYPE_PONG, TYPE_REQ, TYPE_RSP,
                        encode_frame, fnv1a)

ASSET_JPEG = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "..", "..", "assets", "camera", "frame_640x480.jpg")

CAMERA_SIZES = {(480, 480), (640, 480), (864, 480)}

# The DAC's digital gain field, from bk7258_aud.h: six bits, 0 dB at 0x2d.  So
# unity is 714 of 1000 and not the top of the range -- above it the DAC is
# amplifying.
DAC_GAIN_MAX = 0x3f
DAC_GAIN_0DB = 0x2d

# Conversation history fixture, copied from what is actually on the card so the
# awkward cases are present rather than invented:
#
#   - record 5 is unable_to_judge with a confidence below any useful threshold,
#     which is the record a page is most likely to render as a blank row;
#   - record 4's summary mentions 爬山 and its transcript does not, so a keyword
#     search that only reads transcripts silently misses it;
#   - the dates are not contiguous, so an off-by-one in a range filter shows up.
CONV_FIXTURE = [
    {"seq": 1, "date": 20260805, "epoch": 1786060800, "duration_ms": 8000,
     "cue": "brow_furrow", "confidence": 0.72, "unable_to_judge": False,
     "text_bytes": 174, "summary": "周末爬山的安排",
     "text": "对方: 这周末要不要去爬山\n我: 可以，几点出发\n"
             "对方: 六点吧，太晚了太阳大\n我: 那我带点水和面包\n"
             "对方: 好，我带防晒",
     "analysis": {"schema": "social-cue/v1",
                  "cues": [{"cue": "brow_furrow",
                            "meaning": "possible concern",
                            "confidence": 0.72}],
                  "overall_confidence": 0.72,
                  "unable_to_judge": False, "reason": "",
                  "suggestion": "check whether the early start is a problem"}},
    {"seq": 2, "date": 20260809, "epoch": 1786406400, "duration_ms": 12000,
     "cue": "gaze_aversion", "confidence": 0.64, "unable_to_judge": False,
     "text_bytes": 201, "summary": "项目排期的分歧",
     "text": "对方: 这个排期我觉得太紧了\n我: 哪一部分\n"
             "对方: 测试只留了两天\n我: 那我们把联调提前一周\n"
             "对方: 那还行",
     "analysis": {"schema": "social-cue/v1",
                  "cues": [{"cue": "gaze_aversion",
                            "meaning": "possible discomfort",
                            "confidence": 0.64}],
                  "overall_confidence": 0.64,
                  "unable_to_judge": False, "reason": "",
                  "suggestion": "ask which part feels tight before defending"}},
    {"seq": 3, "date": 20260812, "epoch": 1786665600, "duration_ms": 6000,
     "cue": "smile", "confidence": 0.81, "unable_to_judge": False,
     "text_bytes": 117, "summary": "确认了下周的评审时间",
     "text": "对方: 下周三下午评审可以吗\n我: 可以\n对方: 那我发日历",
     "analysis": {"schema": "social-cue/v1",
                  "cues": [{"cue": "smile", "meaning": "possible agreement",
                            "confidence": 0.81}],
                  "overall_confidence": 0.81,
                  "unable_to_judge": False, "reason": "", "suggestion": ""}},
    {"seq": 4, "date": 20260814, "epoch": 1786838400, "duration_ms": 15000,
     "cue": "head_shake", "confidence": 0.69, "unable_to_judge": False,
     "text_bytes": 192, "summary": "关于爬山路线的不同意见",
     "text": "对方: 我还是觉得走西边那条路线不合适\n我: 为什么呢\n"
             "对方: 那边石头多，下雨天滑\n我: 那我们走东边，虽然远一点\n"
             "对方: 远点没关系，安全重要",
     "analysis": {"schema": "social-cue/v1",
                  "cues": [{"cue": "head_shake",
                            "meaning": "possible disagreement",
                            "confidence": 0.69}],
                  "overall_confidence": 0.69,
                  "unable_to_judge": False, "reason": "",
                  "suggestion": "ask what specifically does not work before "
                                "restating"}},
    {"seq": 5, "date": 20260816, "epoch": 1787011200, "duration_ms": 4000,
     "cue": "", "confidence": 0.31, "unable_to_judge": True,
     "text_bytes": 57, "summary": "画面不完整，未能判断",
     "text": "对方: 那个东西你放哪了\n我: 在抽屉里",
     "analysis": {"schema": "social-cue/v1", "cues": [],
                  "overall_confidence": 0.31, "unable_to_judge": True,
                  "reason": "face partially out of frame",
                  "suggestion": "no reading offered"}},
    {"seq": 6, "date": 20260818, "epoch": 1787184000, "duration_ms": 9000,
     "cue": "lean_forward", "confidence": 0.77, "unable_to_judge": False,
     "text_bytes": 174, "summary": "聊到爬山装备",
     "text": "对方: 你那双鞋是什么牌子的\n我: 就普通的登山鞋\n"
             "对方: 抓地怎么样\n我: 还行，湿石头上也稳\n对方: 那我也去看看",
     "analysis": {"schema": "social-cue/v1",
                  "cues": [{"cue": "lean_forward",
                            "meaning": "possible interest",
                            "confidence": 0.77}],
                  "overall_confidence": 0.77,
                  "unable_to_judge": False, "reason": "",
                  "suggestion": ""}},
]


def _errname(err: int) -> str:
    return {2: "ENOENT", 16: "EBUSY", 22: "EINVAL", 38: "ENOSYS",
            12: "ENOMEM", 7: "E2BIG"}.get(abs(err), "EUNKNOWN")


def ok(data) -> bytes:
    return json.dumps({"ok": True, "data": data}).encode()


def fail(msg: str, err: int) -> bytes:
    return json.dumps({"ok": False, "err": msg, "errno": -abs(err),
                       "errname": _errname(err)}).encode()


class MockBoard:
    def __init__(self, args) -> None:
        self.args = args
        self.kvdb = {
            "llm.host": "token-plan-cn.example.com",
            "llm.model": "mimo-v2.5",
            "llm.key": "sk-abcdefghijklmnopqrstuvwxyz0123456789",
            "wifi.ssid": "MockPublic",
            "web.allow": "127.0.0.1",
        }
        self.log_on = False
        self.camera = False
        self.cam_task = None
        self.log_task = None
        self.frames_sent = 0
        self.frames_dropped = 0
        self.shell_running = False
        self.dac_gain = DAC_GAIN_0DB      # the device comes up at 0 dB
        # Empty card: the clip has to be uploaded once, so the tests start from
        # the state a fresh board is actually in.
        self.announce_clip = b""
        self.seq = 0
        self.t0 = time.monotonic()
        self.backlog = ["mock: boot line %d" % i for i in range(5)]
        self.jpeg = self._load_jpeg()
        self.writer = None
        self.reboots = 0
        # A real board is dark for a couple of seconds after a reset, which is
        # the interval the reconnect logic has to survive.
        self.down_until = 0.0

    def _load_jpeg(self) -> bytes:
        try:
            with open(ASSET_JPEG, "rb") as fp:
                return fp.read()
        except OSError:
            # Still SOI/EOI-correct, so verification passes; only the picture
            # is missing, and nothing here is about the picture.
            return b"\xff\xd8" + b"\x00" * 20000 + b"\xff\xd9"

    def now_ms(self) -> int:
        return int((time.monotonic() - self.t0) * 1000)

    # ---- sending -------------------------------------------------------

    async def send(self, ftype: int, req_id: int, payload: bytes) -> None:
        if self.writer is None:
            return
        try:
            self.writer.write(encode_frame(ftype, req_id, payload))
            await self.writer.drain()
        except (ConnectionError, RuntimeError):
            self.writer = None

    # ---- background producers ------------------------------------------

    async def _camera_loop(self) -> None:
        """5 fps, the rate the driver paces at.  No pacing anywhere else."""
        n = 0
        try:
            while self.camera:
                await asyncio.sleep(0.2)
                jpeg = self.jpeg
                digest = fnv1a(jpeg)
                if self.args.corrupt and (n % self.args.corrupt) == 0 and n:
                    # Same length, wrong checksum: exactly the shape of the
                    # bitstream defect this project already had once, where
                    # every marker was legal and only the content was wrong.
                    digest ^= 0xFFFFFFFF
                payload = struct.pack("<II", self.seq, digest) + jpeg
                await self.send(TYPE_EVT_FRAME, 0, payload)
                self.frames_sent += 1
                self.seq += 1
                n += 1
        except asyncio.CancelledError:
            pass

    async def _log_loop(self) -> None:
        rate = self.args.flood or 2
        n = 0
        try:
            while True:
                await asyncio.sleep(1.0 / rate)
                if not self.log_on:
                    continue
                await self.send(
                    TYPE_EVT_LOG, 0,
                    json.dumps({"t": self.now_ms(),
                                "line": "mock: tick %d" % n}).encode())
                n += 1
                if self.args.flood and n % 20 == 0:
                    # The board reports gaps rather than hiding them; the page
                    # has to show a dropped counter, so the mock produces one.
                    await self.send(
                        TYPE_RSP, 0,
                        json.dumps({"t": self.now_ms(), "dropped": 3}).encode())
        except asyncio.CancelledError:
            pass

    # ---- command handling ----------------------------------------------

    def _mask(self, key: str, value: str, raw: bool):
        secret = key.endswith(".key") or key.endswith(".psk")
        if raw or not secret or not value:
            return value, False
        if len(value) <= 8:
            return "**** (%d bytes)" % len(value), True
        return "%s...%s (%d bytes)" % (value[:4], value[-4:], len(value)), True

    async def handle(self, req_id: int, body: bytes) -> None:
        try:
            req = json.loads(body.decode())
        except json.JSONDecodeError:
            await self.send(TYPE_RSP, req_id,
                            fail("request is not valid JSON", 22))
            return

        cmd = req.get("cmd", "")
        args = req.get("args") or {}
        raw = bool(args.get("raw"))

        if cmd == "kvdb.list":
            items = []
            for k, v in self.kvdb.items():
                shown, masked = self._mask(k, v, raw)
                items.append({"key": k, "value": shown, "masked": masked})
            rsp = ok({"items": items, "persistent": True})
        elif cmd == "kvdb.get":
            key = args.get("key")
            if key not in self.kvdb:
                rsp = fail("kvdb: key not found", 2)
            else:
                shown, masked = self._mask(key, self.kvdb[key], raw)
                rsp = ok({"key": key, "value": shown, "masked": masked})
        elif cmd == "kvdb.set":
            self.kvdb[args["key"]] = args["value"]
            rsp = ok({"persistent": True})
        elif cmd == "kvdb.del":
            if self.kvdb.pop(args.get("key"), None) is None:
                rsp = fail("kvdb: key not found", 2)
            else:
                rsp = ok({})
        elif cmd == "wifi.status":
            rsp = ok({"running": True, "ssid": self.kvdb.get("wifi.ssid", ""),
                      "ip": "127.0.0.1", "netmask": "255.255.254.0",
                      "gw": "10.192.104.1", "flags": 5})
        elif cmd == "wifi.connect":
            self.kvdb["wifi.ssid"] = args.get("ssid", "")
            rsp = ok({"running": True, "ip": "127.0.0.1",
                      "ssid": args.get("ssid", ""),
                      "netmask": "255.255.254.0", "gw": "10.192.104.1"})
        elif cmd == "camera.start":
            w = int(args.get("width", 640))
            h = int(args.get("height", 480))
            if (w, h) not in CAMERA_SIZES:
                rsp = fail("camera: size must be 480x480, 640x480 or 864x480",
                           22)
            elif self.camera:
                rsp = fail("camera already streaming", 16)
            else:
                self.camera = True
                self.frames_sent = 0
                self.frames_dropped = 0
                self.cam_task = asyncio.create_task(self._camera_loop())
                rsp = ok({})
        elif cmd == "camera.stop":
            self.camera = False
            if self.cam_task is not None:
                self.cam_task.cancel()
                self.cam_task = None
            rsp = ok({"frames_sent": self.frames_sent,
                      "frames_dropped": self.frames_dropped})
        elif cmd == "log.subscribe":
            on = bool(args.get("on", True))
            replayed = 0
            if on and not self.log_on:
                for line in self.backlog:
                    await self.send(
                        TYPE_EVT_LOG, 0,
                        json.dumps({"t": self.now_ms(),
                                    "line": line}).encode())
                    replayed += 1
            self.log_on = on
            rsp = ok({"replayed": replayed})
        elif cmd == "sys.status":
            rsp = ok({
                "heaps": [
                    {"name": "Umem", "total": 6508992, "used": 250256,
                     "free": 6258736, "maxfree": 6291440},
                    {"name": "psram-display", "total": 5701632, "used": 392,
                     "free": 5701240, "maxfree": 5701240},
                ],
                "tasks": 12,
                "uptime": round(time.monotonic() - self.t0, 2),
                "log_buffered": 0,
                "camera": self.camera,
                "shell": self.shell_running,
            })
        elif cmd == "sys.reboot":
            rsp = ok({})
            self.reboots += 1
            asyncio.create_task(self._reboot())
        elif cmd == "shell.exec":
            if self.shell_running:
                rsp = fail("busy", 16)
            else:
                self.shell_running = True
                asyncio.create_task(self._shell(args.get("cmdline", "")))
                rsp = ok({"accepted": True})
        elif cmd == "shell.kill":
            self.shell_running = False
            rsp = ok({})
        elif cmd == "audio.volume":
            rsp = self._audio_volume(args)
        elif cmd == "audio.announce":
            rsp = self._audio_announce(args)
        elif cmd == "conv.query":
            rsp = self._conv_query(args)
        elif cmd == "conv.get":
            rsp = self._conv_get(args)
        else:
            rsp = fail("unknown cmd", 38)

        await self.send(TYPE_RSP, req_id, rsp)

    # ---- playback volume -----------------------------------------------

    def _audio_announce(self, args: dict) -> bytes:
        """The confirmation clip: upload with b64, query without.

        The offset is honoured rather than ignored so a test can prove the
        chunks are reassembled in order; a mock that just appended would pass
        even if the page sent them backwards.
        """
        b64 = args.get("b64")
        if b64 is None:
            if not self.announce_clip:
                return ok({"present": False, "bytes": 0})
            n = len(self.announce_clip)
            return ok({"present": True, "bytes": n,
                       "seconds": round(n / 2 / 8000, 2)})

        offset = args.get("offset", 0)
        if not isinstance(offset, int) or offset < 0:
            return fail("audio.announce: offset out of range", 22)

        try:
            raw = base64.b64decode(b64, validate=True)
        except (ValueError, binascii.Error):
            return fail("audio.announce: chunk is not valid base64", 22)

        if offset == 0:
            self.announce_clip = b""
        if offset != len(self.announce_clip):
            # The board seeks, so a gap would leave a hole; saying so is what
            # lets the page retry rather than ship a file with a hole in it.
            return fail("audio.announce: offset does not follow the file", 22)

        self.announce_clip += raw
        return ok({"bytes": len(self.announce_clip),
                   "final": bool(args.get("final"))})

    def _audio_volume(self, args: dict) -> bytes:
        """The DAC's 6-bit digital gain, quantised the way the driver does.

        The quantisation is reproduced rather than smoothed over because it is
        the surprising part: the board answers 698 to a request of 700, and a
        mock that echoed the request would let a page ship that displays the
        request as though the hardware had confirmed it.
        """
        want = args.get("volume")
        if want is not None:
            if not isinstance(want, int) or want < 0 or want > 1000:
                return fail("audio.volume: volume must be 0..1000", 22)
            self.dac_gain = (want * DAC_GAIN_MAX + 500) // 1000

        # Rounded both ways, matching the driver: truncating loses a step in
        # each direction and the two compound, so 0 dB could not be asked for
        # by the value the board itself reported.
        volume = ((self.dac_gain * 1000 + DAC_GAIN_MAX // 2) // DAC_GAIN_MAX)

        # The announcement only happens on a set, and only when there is a clip
        # to play.  Reported apart from ok:true because the volume did take
        # either way -- folding a missing clip into a failure would say the
        # setting was lost when it was not.
        announced = False
        rsp = {"volume": volume,
               "percent": (volume * 100 + 500) // 1000,
               "unity": ((DAC_GAIN_0DB * 1000 + DAC_GAIN_MAX // 2) //
                         DAC_GAIN_MAX),
               "max": 1000,
               "applied": want is not None}

        if want is not None and args.get("announce", True):
            if self.announce_clip:
                announced = True
            else:
                rsp["announce_errno"] = -2
                rsp["announce_errname"] = "ENOENT"

        rsp["announced"] = announced
        return ok(rsp)

    # ---- conversation history ------------------------------------------

    def _conv_query(self, args: dict) -> bytes:
        """Same two-stage filter the board runs, on the fixture above.

        Reimplemented rather than stubbed to a fixed list: the point of the
        filters is which records they leave out, and a mock that returned
        everything would let a page that ignores `from`/`to`/`keyword` pass.
        The keyword searches summary *and* transcript, which is the part that
        is easy to get wrong in only one of the two places -- record 4's
        summary mentions 爬山 and its transcript does not.
        """
        frm = args.get("from") or 0
        to = args.get("to") or 0
        cue = args.get("cue") or ""
        keyword = args.get("keyword") or ""
        include_unjudged = args.get("include_unjudged", True)
        limit = args.get("limit") or 50
        if not isinstance(limit, int) or limit <= 0 or limit > 200:
            limit = 50

        matched = 0
        items = []
        for rec in CONV_FIXTURE:
            if frm and rec["date"] < frm:
                continue
            if to and rec["date"] > to:
                continue
            if cue and rec["cue"] != cue:
                continue
            if rec["unable_to_judge"] and not include_unjudged:
                continue
            if float(args.get("min_confidence") or 0.0) > rec["confidence"]:
                continue
            if keyword and keyword not in rec["summary"] \
                    and keyword not in rec["text"]:
                continue

            matched += 1
            if len(items) >= limit:
                continue
            items.append({k: rec[k] for k in
                          ("seq", "date", "epoch", "duration_ms", "cue",
                           "confidence", "unable_to_judge", "text_bytes",
                           "summary")})

        return ok({"items": items, "matched": matched,
                   "returned": len(items), "limit": limit})

    def _conv_get(self, args: dict) -> bytes:
        seq = args.get("seq") or 0
        for rec in CONV_FIXTURE:
            if rec["seq"] == seq:
                return ok({"seq": seq, "text": rec["text"],
                           "text_bytes": rec["text_bytes"],
                           "cue": rec["analysis"]})
        if not seq:
            return fail("conv.get: seq is required", 22)
        return fail("conv.get: no such record", 2)

    async def _shell(self, cmdline: str) -> None:
        burst = 40 if self.args.shell_burst else 3
        for i in range(burst):
            await asyncio.sleep(0.005 if self.args.shell_burst else 0.05)
            await self.send(TYPE_EVT_LOG, 0,
                            json.dumps({"t": self.now_ms(),
                                        "line": "%s: line %d"
                                                % (cmdline, i)}).encode())
        # As an RSP, not an EVT_LOG: the board sends it that way because the
        # exit notice must survive a burst that fills the send queue.
        await self.send(TYPE_RSP, 0,
                        json.dumps({"t": self.now_ms(), "exit": 0}).encode())
        self.shell_running = False

    async def _reboot(self) -> None:
        await asyncio.sleep(0.2)
        self.down_until = time.monotonic() + self.args.reboot_downtime
        if self.writer is not None:
            try:
                self.writer.close()
            except Exception:                          # noqa: BLE001
                pass
            self.writer = None
        self.camera = False
        self.log_on = False

    # ---- server --------------------------------------------------------

    async def _client(self, reader, writer) -> None:
        peer = writer.get_extra_info("peername")
        if self.args.refuse or time.monotonic() < self.down_until:
            reason = "refused" if self.args.refuse else "rebooting"
            print("mock: %s %s" % (reason, peer), flush=True)
            writer.close()
            return

        if self.writer is not None:
            # One client at a time; a new one displaces the old, as the board
            # does, so two subscriptions and two camera lifetimes cannot exist.
            print("mock: displacing previous client", flush=True)
            try:
                self.writer.close()
            except Exception:                          # noqa: BLE001
                pass

        self.writer = writer
        print("mock: client %s connected" % (peer,), flush=True)
        if self.log_task is None:
            self.log_task = asyncio.create_task(self._log_loop())

        parser = FrameParser()
        try:
            while True:
                chunk = await reader.read(65536)
                if not chunk:
                    break
                for ftype, req_id, payload in parser.feed(chunk):
                    if ftype == TYPE_PING:
                        await self.send(TYPE_PONG, req_id, b"")
                    elif ftype == TYPE_REQ:
                        await self.handle(req_id, payload)
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            if self.writer is writer:
                self.writer = None
            self.camera = False
            self.log_on = False
            print("mock: client disconnected", flush=True)

    async def connect_out(self) -> None:
        """Dial the console over TLS, as the real board does.

        This is the production direction: the board is behind the access
        point's NAT, so it has to call us.  The mock does the same two things
        the board does on connect -- verify the server certificate against a
        pinned SHA-256, then announce itself with the shared token -- so the
        console's half of that exchange is exercised by the tests instead of
        only on hardware.
        """
        import hashlib
        import ssl as _ssl

        while True:
            try:
                # No chain verification, exactly like the board: there is no CA
                # and no clock.  The pin below is what actually decides.
                ctx = _ssl.SSLContext(_ssl.PROTOCOL_TLS_CLIENT)
                ctx.check_hostname = False
                ctx.verify_mode = _ssl.CERT_NONE
                ctx.minimum_version = _ssl.TLSVersion.TLSv1_2
                ctx.maximum_version = _ssl.TLSVersion.TLSv1_2

                host, _, port = self.args.connect.rpartition(":")
                reader, writer = await asyncio.open_connection(
                    host, int(port), ssl=ctx)

                der = writer.get_extra_info("ssl_object").getpeercert(True)
                seen = hashlib.sha256(der).hexdigest()
                if self.args.pin_fp and seen != self.args.pin_fp.lower():
                    print("mock: fingerprint mismatch\n  pinned %s\n  seen   %s"
                          % (self.args.pin_fp, seen), flush=True)
                    writer.close()
                    return
                print("mock: TLS to %s:%s, fingerprint %s"
                      % (host, port, seen), flush=True)

                self.writer = writer
                await self.send(TYPE_HELLO, 0, json.dumps(
                    {"token": self.args.token, "proto": 1}).encode())
                if self.log_task is None:
                    self.log_task = asyncio.create_task(self._log_loop())

                parser = FrameParser()
                while True:
                    chunk = await reader.read(65536)
                    if not chunk:
                        break
                    for ftype, req_id, payload in parser.feed(chunk):
                        if ftype == TYPE_PING:
                            await self.send(TYPE_PONG, req_id, b"")
                        elif ftype == TYPE_REQ:
                            await self.handle(req_id, payload)
            except (ConnectionError, OSError, _ssl.SSLError) as exc:
                print("mock: connect failed: %s" % exc, flush=True)
            finally:
                self.writer = None
                self.camera = False
                self.log_on = False

            if self.args.once:
                return
            await asyncio.sleep(0.5)

    async def serve_tcp(self) -> None:
        server = await asyncio.start_server(self._client, self.args.bind,
                                            self.args.port)
        addr = ", ".join(str(s.getsockname()) for s in server.sockets)
        print("mock: listening on %s" % addr, flush=True)
        async with server:
            await server.serve_forever()

    # ---- pty side (NSH text) -------------------------------------------

    def serve_pty(self) -> str:
        """A pty that answers like the AP console.  Returns the slave path.

        This is what makes the four bootstrap paths testable: the backend runs
        its real serial code, including the fuser check and the termios setup,
        against something that behaves like the console.
        """
        master, slave = pty.openpty()
        path = os.ttyname(slave)
        loop = asyncio.get_event_loop()

        state = {"provisioned": self.args.scenario == "running",
                 "associated": self.args.scenario == "running",
                 "renews": 0}

        def reply(text: str) -> None:
            os.write(master, text.replace("\n", "\r\n").encode())

        def ifconfig() -> str:
            if state["provisioned"]:
                return ("wlan0\tLink encap:Ethernet HWaddr "
                        "c8:47:8c:55:6b:ea at RUNNING mtu 1500\n"
                        "\tinet addr:%s DRaddr:10.192.104.1 "
                        "Mask:255.255.254.0\n" % self.args.board_ip)
            return ("wlan0\tLink encap:Ethernet HWaddr "
                    "c8:47:8c:55:6b:ea at UP mtu 1500\n"
                    "\tinet addr:10.0.0.2 DRaddr:10.0.0.1 "
                    "Mask:255.255.255.0\n")

        buf = bytearray()

        def on_readable() -> None:
            try:
                data = os.read(master, 1024)
            except OSError:
                return
            buf.extend(data)
            while b"\n" in buf:
                idx = buf.index(b"\n")
                line = bytes(buf[:idx]).decode("utf-8", "replace").strip()
                del buf[:idx + 1]
                line = line.replace("\x1d", "").replace(".", "", 0)
                if not line:
                    reply("nsh> ")
                    continue
                if line.startswith("ap_console"):
                    reply("ap_bridg: link up\nnsh> ")
                elif line.startswith("ifconfig"):
                    reply(ifconfig() + "nsh> ")
                elif line.startswith("ifup"):
                    reply("nsh> ")
                elif line.startswith("wapi psk"):
                    reply("nsh> ")
                elif line.startswith("wapi essid"):
                    state["associated"] = self.args.scenario != "no-network"
                    reply("nsh> ")
                elif line.startswith("renew"):
                    state["renews"] += 1
                    # The first renew after association fails on real hardware;
                    # a mock that always succeeded would let a backend without
                    # the retry pass.
                    if state["associated"] and state["renews"] >= 2:
                        state["provisioned"] = True
                        reply("nsh> ")
                    else:
                        reply("ERROR: netlib_obtain_ipv4addr() failed\nnsh> ")
                elif line.startswith("kvdb"):
                    reply("kvdb: ok\nnsh> ")
                elif line.startswith("reboot"):
                    reply("nsh> ")
                else:
                    reply("nsh: %s: command not found\nnsh> " % line)

        loop.add_reader(master, on_readable)
        print("mock: pty at %s" % path, flush=True)
        return path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=8888)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--refuse", action="store_true",
                    help="refuse every connection, as the whitelist does")
    ap.add_argument("--corrupt", type=int, default=0,
                    help="break the checksum of every Nth frame")
    ap.add_argument("--flood", type=int, default=0,
                    help="log lines per second")
    ap.add_argument("--scenario", default="running",
                    choices=("running", "needs-setup", "no-network"))
    ap.add_argument("--board-ip", default="127.0.0.1")
    ap.add_argument("--reboot-downtime", type=float, default=2.0)
    ap.add_argument("--pty", action="store_true",
                    help="also serve an NSH-like console on a pty")
    ap.add_argument("--pty-path-file", default="",
                    help="write the pty path here, for test scripts")
    ap.add_argument("--connect", default="",
                    help="dial host:port over TLS instead of listening, which "
                         "is what the real board does from behind NAT")
    ap.add_argument("--token", default="",
                    help="value for the HELLO frame")
    ap.add_argument("--pin-fp", default="",
                    help="server certificate SHA-256 to require")
    ap.add_argument("--once", action="store_true",
                    help="do not reconnect after the session ends")
    ap.add_argument("--shell-burst", action="store_true",
                    help="emit a long burst of shell output, the case that "
                         "used to lose the exit notice")
    args = ap.parse_args()

    board = MockBoard(args)

    async def run() -> None:
        if args.pty:
            path = board.serve_pty()
            if args.pty_path_file:
                with open(args.pty_path_file, "w", encoding="utf-8") as fp:
                    fp.write(path)
        if args.connect:
            await board.connect_out()
        else:
            await board.serve_tcp()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
