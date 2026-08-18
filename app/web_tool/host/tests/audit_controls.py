#!/usr/bin/env python3
"""Exercise every control the page can trigger, and say which ones fail.

Written because "some buttons do not work" is not something to answer by
reading code: the page reaches the board through two hops and a NAT, and any of
the backend op, the board command, or the board's own state can be the one that
is broken.  This drives all of them in one pass and prints a verdict per
control, so the answer is a list rather than a guess.

Usage: ./audit_controls.py [--url https://127.0.0.1:8443] [--ssid X --psk Y]
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import ssl
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import tlsconf                                          # noqa: E402

RESULTS: list[tuple[str, bool, str]] = []


def record(control: str, ok: bool, detail: str) -> None:
    RESULTS.append((control, ok, detail))
    print("[%s] %-26s %s" % ("ok  " if ok else "FAIL", control, detail),
          flush=True)


class Page:
    """The page's side of the WebSocket, with nothing else in the way."""

    def __init__(self, ws) -> None:
        self.ws = ws
        self.next_id = 100
        self.logs: list = []
        self.frames: list = []
        self.states: list = []

    async def _pump(self, want_id, timeout):
        import aiohttp
        end = time.time() + timeout
        while time.time() < end:
            try:
                m = await asyncio.wait_for(self.ws.receive(),
                                           timeout=max(0.2, end - time.time()))
            except asyncio.TimeoutError:
                return None
            if m.type is aiohttp.WSMsgType.BINARY:
                self.frames.append(m.data)
                continue
            if m.type is not aiohttp.WSMsgType.TEXT:
                return None
            o = json.loads(m.data)
            t = o.get("type")
            if t == "log":
                self.logs.append(o["event"])
            elif t == "state":
                self.states.append(o["state"])
            elif t == "rsp" and (want_id is None or o.get("id") == want_id):
                return o
        return None

    async def op(self, payload: dict, timeout: float = 30.0):
        self.next_id += 1
        payload = dict(payload, id=self.next_id)
        await self.ws.send_str(json.dumps(payload))
        return await self._pump(self.next_id, timeout)

    async def cmd(self, name: str, args: dict | None = None,
                  timeout: float = 30.0):
        return await self.op({"op": "cmd", "cmd": name, "args": args or {}},
                             timeout)

    async def drain(self, seconds: float) -> None:
        await self._pump(None, seconds)


async def audit(args) -> int:
    import aiohttp

    ctx = ssl.create_default_context(cafile=tlsconf.CERT_PATH)
    ctx.check_hostname = False
    conn = aiohttp.TCPConnector(ssl=ctx)

    async with aiohttp.ClientSession(connector=conn) as sess:
        # ---- the page itself -------------------------------------------
        async with sess.get(args.url + "/") as r:
            body = await r.text()
        record("GET / (HTTPS)", r.status == 200, "%d, %d bytes"
               % (r.status, len(body)))
        for asset in ("style.css", "app.js", "keystore.mjs"):
            async with sess.get(args.url + "/static/" + asset) as r:
                await r.read()
            record("GET /static/" + asset, r.status == 200, str(r.status))

        async with sess.ws_connect(args.url + "/ws") as ws:
            page = Page(ws)
            first = await page._pump(None, 10)
            record("WSS opens, pushes state",
                   first is None and bool(page.states) or first is not None,
                   "state received" if page.states else "no state")

            st = page.states[-1] if page.states else {}
            link = st.get("link", {})
            connected = bool(link.get("connected"))
            record("board link", connected,
                   "%s %s" % (link.get("mode"), link.get("host")
                              or link.get("last_error")))

            # ---- pairing, if the board is not there ---------------------
            if not connected and args.ssid:
                r = await page.op({"op": "pair", "ssid": args.ssid,
                                   "psk": args.psk}, timeout=180)
                ok = bool(r and r.get("ok"))
                record("一键配对 (op pair)", ok,
                       (r or {}).get("data", {}).get("err")
                       or (r or {}).get("data", {}).get("ip") or "no answer")
                # give the board time to dial back in
                for _ in range(40):
                    await page.drain(1.0)
                    s2 = await page.op({"op": "state"})
                    if s2 and s2["data"]["link"]["connected"]:
                        connected = True
                        break
                record("board dials back in", connected, "")

            # ---- ops that do not need the board ------------------------
            r = await page.op({"op": "state"})
            record("op state", bool(r and r.get("ok")), "")

            r = await page.op({"op": "pairing"})
            cmds = (r or {}).get("data", {}).get("commands", [])
            record("op pairing", bool(cmds), "%d command(s)" % len(cmds))

            r = await page.op({"op": "capture", "action": "start"})
            capdir = (r or {}).get("data", {}).get("dir", "")
            record("开始落盘 (capture start)", bool(r and r.get("ok")),
                   os.path.basename(capdir))

            r = await page.op({"op": "capture", "action": "stop"})
            record("停止落盘 (capture stop)", bool(r and r.get("ok")), "")

            r = await page.op({"op": "serial", "action": "acquire"})
            acq = bool(r and r.get("ok"))
            record("占用串口 (serial acquire)", acq,
                   (r or {}).get("err", "held"))
            r = await page.op({"op": "serial", "action": "release"})
            record("释放串口 (serial release)", bool(r and r.get("ok")),
                   "holder=%s" % (r or {}).get("holder"))

            r = await page.op({"op": "nonsense"})
            record("unknown op refused", bool(r and r.get("ok") is False),
                   (r or {}).get("err", ""))

            if not connected:
                record("board commands", False,
                       "skipped: no board.  Pass --ssid/--psk to pair.")
                return summarise()

            # ---- board commands ----------------------------------------
            r = await page.cmd("sys.status")
            record("系统状态 (sys.status)", bool(r and r.get("ok")),
                   "uptime %s" % (r or {}).get("data", {}).get("uptime"))

            r = await page.cmd("kvdb.list")
            n = len((r or {}).get("data", {}).get("items", []))
            record("读回 (kvdb.list)", bool(r and r.get("ok")),
                   "%d key(s)" % n)

            r = await page.cmd("kvdb.set", {"key": "audit.probe",
                                            "value": "v"})
            record("写入板子 (kvdb.set)", bool(r and r.get("ok")), "")
            r = await page.cmd("kvdb.del", {"key": "audit.probe"})
            record("kvdb.del", bool(r and r.get("ok")), "")

            r = await page.cmd("wifi.status")
            record("查看状态 (wifi.status)", bool(r and r.get("ok")),
                   (r or {}).get("data", {}).get("ip", ""))

            r = await page.cmd("log.subscribe", {"on": True})
            record("订阅 (log.subscribe on)", bool(r and r.get("ok")),
                   "replayed %s" % (r or {}).get("data", {}).get("replayed"))

            # console: a command, its output, and its exit notice
            before = len(page.logs)
            r = await page.cmd("shell.exec", {"cmdline": "free"})
            ok = bool(r and r.get("ok"))
            await page.drain(8.0)
            lines = [e for e in page.logs[before:] if "line" in e]
            exits = [e for e in page.logs[before:] if "exit" in e]
            record("控制台执行 (shell.exec)", ok and bool(lines),
                   "%d line(s)" % len(lines))
            record("命令结束通知 (exit)", bool(exits),
                   json.dumps(exits[-1]) if exits else "MISSING")

            r = await page.cmd("shell.kill")
            record("停止当前命令 (shell.kill)", bool(r and r.get("ok")), "")

            # camera: both geometries, measured
            for w, h in ((480, 480), (640, 480)):
                page.frames.clear()
                r = await page.cmd("camera.start",
                                   {"width": w, "height": h}, timeout=90)
                if not (r and r.get("ok")):
                    record("预览 %dx%d (camera.start)" % (w, h), False,
                           json.dumps(r))
                    continue
                t0 = time.time()
                await page.drain(args.camera_seconds)
                span = time.time() - t0
                stop = await page.cmd("camera.stop", timeout=40)
                n = len(page.frames)
                fps = (n - 1) / span if n > 1 else 0.0
                record("预览 %dx%d" % (w, h), n > 1,
                       "%d frames, %.2f fps, board=%s"
                       % (n, fps, json.dumps((stop or {}).get("data"))))
                await asyncio.sleep(2.0)

            r = await page.cmd("camera.stop")
            record("停止 (camera.stop, idempotent)", bool(r and r.get("ok")),
                   json.dumps((r or {}).get("data")))

            r = await page.cmd("log.subscribe", {"on": False})
            record("取消订阅 (log.subscribe off)", bool(r and r.get("ok")), "")

            r = await page.cmd("nonsense.cmd")
            record("unknown cmd refused",
                   bool(r and r.get("errname") == "ENOSYS"), "")

    return summarise()


def summarise() -> int:
    bad = [c for c, ok, _ in RESULTS if not ok]
    print("\n%d control(s), %d failing" % (len(RESULTS), len(bad)))
    for c in bad:
        print("  - %s" % c)
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="https://127.0.0.1:8443")
    ap.add_argument("--ssid", default="")
    ap.add_argument("--psk", default="")
    ap.add_argument("--camera-seconds", type=float, default=12.0)
    args = ap.parse_args()
    return asyncio.run(audit(args))


if __name__ == "__main__":
    sys.exit(main())
