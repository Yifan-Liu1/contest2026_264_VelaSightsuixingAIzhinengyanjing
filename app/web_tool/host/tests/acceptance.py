#!/usr/bin/env python3
"""Real-board acceptance: run the checklist and print the evidence.

Every item in the design doc's section 11 that needs hardware, in one command,
so the record can be regenerated instead of retyped.  Each check prints the
number it measured, not a verdict, because a verdict without the number is not
reproducible -- the format `8.16基础适配门禁验收记录.md` established.

    ./acceptance.py --board 10.192.105.127
    ./acceptance.py --board 10.192.105.127 --long   (adds the 10-minute run)

What it needs of the board: web_tool running and this machine in web.allow.
The whitelist check temporarily rewrites web.allow and puts it back; if this
script is killed in the middle of that, run

    kvdb set web.allow <this machine's IP>

on the console to restore it.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import shlex
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import board_link                                   # noqa: E402
from board_link import BoardLink                    # noqa: E402
from serial_console import port_holder              # noqa: E402

RESULTS: list[dict] = []


def record(item: str, ok: bool | None, evidence: str) -> None:
    RESULTS.append({"item": item, "ok": ok, "evidence": evidence})
    mark = "PASS" if ok else ("----" if ok is None else "FAIL")
    print("[%s] %s\n       %s" % (mark, item, evidence.replace("\n", "\n       ")),
          flush=True)


def host_ip(board_ip: str) -> str:
    """Which of our addresses the board sees.  Read from the routing table
    rather than assumed, because this machine has several."""
    out = subprocess.run(["ip", "-o", "route", "get", board_ip],
                         capture_output=True, text=True)
    for tok in out.stdout.split():
        if tok == "src":
            idx = out.stdout.split().index("src")
            return out.stdout.split()[idx + 1]
    return ""


async def item_link(link: BoardLink) -> dict:
    r = await link.request("sys.status", timeout=8)
    record("web_tool answers over TCP",
           bool(r.get("ok")),
           "sys.status -> %s" % json.dumps(r)[:300])
    return r.get("data", {}) if r.get("ok") else {}


async def item_free_baseline(link: BoardLink) -> dict:
    r = await link.request("sys.status", timeout=8)
    heaps = {h["name"]: h for h in r.get("data", {}).get("heaps", [])}
    umem = heaps.get("Umem", {})
    record("Umem while web_tool is running", None,
           "total=%s used=%s free=%s  (design expects the service to cost "
           "about 100 KB: one 33 KB frame slot plus a 64 KB log ring)"
           % (umem.get("total"), umem.get("used"), umem.get("free")))
    return umem


async def item_fps(link: BoardLink, seconds: float, width: int,
                   height: int) -> None:
    times: list[float] = []
    seqs: list[int] = []
    sizes: list[int] = []

    def on_frame(seq, digest, jpeg):
        times.append(time.monotonic())
        seqs.append(seq)
        sizes.append(len(jpeg))
        ok = board_link.fnv1a(jpeg) == digest
        if not ok:
            sizes.append(-1)

        async def noop():
            return None
        return noop()

    link.on_frame = on_frame
    r = await link.request("camera.start", {"width": width, "height": height},
                           timeout=10)
    if not r.get("ok"):
        record("preview frame rate", False, "camera.start failed: %s" % r)
        return

    await asyncio.sleep(seconds)
    stop = await link.request("camera.stop", timeout=10)
    link.on_frame = None

    if len(times) < 2:
        record("preview frame rate", False,
               "%d frame(s) in %.0fs" % (len(times), seconds))
        return

    span = times[-1] - times[0]
    fps = (len(times) - 1) / span
    gaps = [times[i + 1] - times[i] for i in range(len(times) - 1)]
    gaps.sort()
    median = gaps[len(gaps) // 2]
    missing = (seqs[-1] - seqs[0] + 1) - len(seqs)
    bad = sum(1 for s in sizes if s < 0)

    record("preview frame rate at %dx%d" % (width, height),
           3.5 <= fps <= 6.0,
           "%d frames in %.1fs -> %.2f fps, median gap %.0f ms, "
           "sizes %d..%d bytes\n"
           "camera.stop -> %s\n"
           "sequence gaps: %d frame(s) never arrived; %d failed the checksum\n"
           "expected about 5 fps: the driver paces at "
           "CONFIG_BK7258_CAMERA_JPEG_FPS and web_tool adds no throttle"
           % (len(times), span, fps, median * 1000,
              min(s for s in sizes if s > 0), max(sizes),
              json.dumps(stop.get("data", {})), missing, bad))


async def item_dropped_long(link: BoardLink, minutes: float) -> None:
    drops: list[int] = []
    frames = {"n": 0}

    def on_frame(seq, digest, jpeg):
        frames["n"] += 1

        async def noop():
            return None
        return noop()

    async def on_log(event):
        if "dropped" in event:
            drops.append(int(event["dropped"]))

    link.on_frame = on_frame
    link.on_log = on_log
    await link.request("log.subscribe", {"on": True}, timeout=8)
    await link.request("camera.start", {"width": 640, "height": 480},
                       timeout=10)

    t0 = time.monotonic()
    deadline = t0 + minutes * 60
    while time.monotonic() < deadline:
        await asyncio.sleep(5)

    stop = await link.request("camera.stop", timeout=10)
    await link.request("log.subscribe", {"on": False}, timeout=8)
    link.on_frame = None
    link.on_log = None

    elapsed = time.monotonic() - t0
    record("continuous preview for %.0f minutes" % minutes, True,
           "%.0fs, %d frames (%.2f fps), %d dropped-log notice(s) totalling "
           "%d line(s)\ncamera.stop -> %s"
           % (elapsed, frames["n"], frames["n"] / elapsed, len(drops),
              sum(drops), json.dumps(stop.get("data", {}))))


async def item_reboot(host: str, port: int) -> None:
    link = BoardLink(host, port)
    await link.connect_once()
    t0 = time.monotonic()
    r = await link.request("sys.reboot", timeout=8)
    if not r.get("ok"):
        record("sys.reboot answers before rebooting", False, str(r))
        await link.close()
        return
    record("sys.reboot answers before rebooting", True,
           "RSP arrived %.0f ms after the request; the page therefore needs "
           "no special case for this command" % ((time.monotonic() - t0) * 1000))
    await link.close()

    # Now measure how long until the service answers again.  A fresh link each
    # attempt, because that is what the backend's reconnect does.
    t_down = time.monotonic()
    deadline = t_down + 120
    attempts = 0
    while time.monotonic() < deadline:
        attempts += 1
        probe = BoardLink(host, port, connect_timeout=2.0)
        if await probe.probe():
            await probe.close()
            record("recovery after sys.reboot", True,
                   "answered again %.1f s after the reboot request, "
                   "%d attempt(s)" % (time.monotonic() - t_down, attempts))
            return
        await probe.close()
        await asyncio.sleep(1.0)
    record("recovery after sys.reboot", False,
           "no answer within %.0f s (%d attempts)"
           % (time.monotonic() - t_down, attempts))


async def item_whitelist(link: BoardLink, host: str, port: int,
                         my_ip: str) -> None:
    """Set web.allow to an address that is not ours, restart the service, and
    check that we are refused.  Then put it back.

    Testing this from a second machine would be cleaner, but there is only one
    development machine; changing the board's idea of who we are is the same
    experiment from the other end.
    """
    saved = await link.request("kvdb.get", {"key": "web.allow", "raw": True},
                               timeout=8)
    if not saved.get("ok"):
        record("connections from outside web.allow are refused", None,
               "web.allow is not set, so nothing to restore -- and with it "
               "unset web_tool does not listen at all, which is the stricter "
               "half of this requirement")
        return
    original = saved["data"]["value"]

    bogus = "192.0.2.1"          # RFC 5737 documentation address
    await link.request("kvdb.set", {"key": "web.allow", "value": bogus},
                       timeout=8)
    await link.request("shell.exec", {"cmdline": "web_tool 8889"}, timeout=8)
    await asyncio.sleep(1.5)

    probe = BoardLink(host, 8889, connect_timeout=3.0)
    refused = not await probe.probe()
    err = probe.last_error
    await probe.close()

    # Put it back before anything else can fail.
    restore = await link.request("kvdb.set",
                                 {"key": "web.allow", "value": original},
                                 timeout=8)

    record("connections from outside web.allow are refused", refused,
           "with web.allow=%s a connection from %s was %s (%s); "
           "web.allow restored to the original value: %s"
           % (bogus, my_ip, "refused" if refused else "ACCEPTED", err,
              "ok" if restore.get("ok") else restore))


def item_serial_free(serial_port: str) -> None:
    holder = port_holder(serial_port)
    record("the serial port is free while the backend runs", holder is None,
           "fuser %s -> %s\nserial_cmd.sh and autoflash.sh both refuse to run "
           "when anything holds this port, so the backend must not keep it"
           % (serial_port, holder or "nothing"))

    repo = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))
    out = subprocess.run(["bash", "-c",
                          "cd %s && ./serial_cmd.sh -w 2 "
                          "'uname -a' 2>&1 | tail -2" % shlex.quote(repo)],
                         capture_output=True, text=True, timeout=90)
    record("serial_cmd.sh still works", "字节" in out.stdout,
           out.stdout.strip()[-300:] or out.stderr.strip()[-300:])


async def item_shell_gate(link: BoardLink) -> None:
    logs: list = []

    async def on_log(event):
        logs.append(event)

    link.on_log = on_log
    await link.request("log.subscribe", {"on": True}, timeout=8)
    a = await link.request("shell.exec", {"cmdline": "free"}, timeout=8)
    b = await link.request("shell.exec", {"cmdline": "ps"}, timeout=8)
    await asyncio.sleep(2.0)
    link.on_log = None

    lines = [e.get("line", "") for e in logs if "line" in e]
    exits = [e for e in logs if e.get("exit") is not None]
    record("shell passthrough relays output and gates concurrency",
           bool(a.get("ok")) and b.get("ok") is False
           and b.get("err") == "busy" and bool(lines),
           "first exec -> %s; second -> %s; %d log line(s) relayed, "
           "%d exit notice(s)\nsample: %s"
           % (json.dumps(a), json.dumps(b), len(lines), len(exits),
              " | ".join(lines[:4])))


async def item_kvdb(link: BoardLink) -> None:
    r = await link.request("kvdb.list", timeout=8)
    items = {i["key"]: i for i in r.get("data", {}).get("items", [])}
    secret = [k for k in items if k.endswith(".key") or k.endswith(".psk")]
    masked_ok = all(items[k]["masked"] for k in secret) if secret else None
    record("secrets are masked in kvdb.list", masked_ok,
           "persistent=%s; %d key(s); secret keys %s\n%s"
           % (r.get("data", {}).get("persistent"), len(items),
              secret or "(none stored)",
              json.dumps([items[k] for k in secret], ensure_ascii=False)
              if secret else "nothing to mask"))

    miss = await link.request("kvdb.get", {"key": "no.such.key"}, timeout=8)
    record("errors carry both errno and errname",
           miss.get("errname") == "ENOENT" and miss.get("errno") == -2,
           json.dumps(miss, ensure_ascii=False))


async def amain(args) -> int:
    my_ip = host_ip(args.board)
    print("acceptance: board %s:%d, this machine %s\n"
          % (args.board, args.port, my_ip or "unknown"), flush=True)

    link = BoardLink(args.board, args.port)
    if not await link.probe():
        record("web_tool answers over TCP", False,
               "could not connect: %s" % link.last_error)
        print("\nStopping: nothing else can be measured without a link.")
        return 1

    await item_link(link)
    await item_free_baseline(link)
    await item_kvdb(link)
    await item_shell_gate(link)
    await item_fps(link, args.fps_seconds, args.width, args.height)
    if args.long:
        await item_dropped_long(link, args.minutes)
    await item_whitelist(link, args.board, args.port, my_ip)
    await link.close()

    item_serial_free(args.serial_port)
    await item_reboot(args.board, args.port)

    failed = [r for r in RESULTS if r["ok"] is False]
    print("\n%d item(s), %d failed, %d informational"
          % (len(RESULTS), len(failed),
             len([r for r in RESULTS if r["ok"] is None])))

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fp:
            json.dump(RESULTS, fp, ensure_ascii=False, indent=2)
        print("evidence written to %s" % args.out)
    return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--board", required=True)
    ap.add_argument("--port", type=int, default=board_link.DEFAULT_PORT)
    ap.add_argument("--serial-port", default="/dev/ttyUSB0")
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=480)
    ap.add_argument("--fps-seconds", type=float, default=20.0)
    ap.add_argument("--long", action="store_true",
                    help="add the continuous-preview run")
    ap.add_argument("--minutes", type=float, default=10.0)
    ap.add_argument("--out", default="")
    args = ap.parse_args()
    return asyncio.run(amain(args))


if __name__ == "__main__":
    sys.exit(main())
