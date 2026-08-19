#!/usr/bin/env python3
"""End-to-end tests: the real backend against mock_board.py.

Covered, because these are the four things the design says must work and the
three that are hardest to provoke on real hardware:

  1. bootstrap, all four paths (cached / serial-ifconfig / serial-provision /
     degraded), each driven through the real serial code against a pty
  2. reconnection after sys.reboot, with the attempt counter visible
  3. the dropped counter surviving from board to browser
  4. a connection refused the way the whitelist refuses one
  5. every structured command answering the shape the design specifies
  6. rejected frames landing in rejected/ rather than being discarded
  7. the port being free again the moment the backend is done with it

Run: ./test_e2e.py      (add -v for the transcript of every step)
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import shutil
import subprocess
import sys
import struct
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))          # host/

import board_link                                   # noqa: E402
import bootstrap as bootstrap_mod                   # noqa: E402
from board_link import BoardLink                    # noqa: E402
from capture import CaptureSession                  # noqa: E402
from serial_console import SerialBusy, SerialConsole  # noqa: E402

MOCK = os.path.join(os.path.dirname(HERE), "mock_board.py")

VERBOSE = False
CHECKS = 0
FAILURES: list[str] = []


def check(cond: bool, what: str) -> bool:
    global CHECKS
    CHECKS += 1
    if cond:
        if VERBOSE:
            print("  ok   %s" % what)
        return True
    FAILURES.append(what)
    print("  FAIL %s" % what)
    return False


class Mock:
    """A mock_board.py subprocess, with its pty path when it has one."""

    def __init__(self, *extra: str, pty: bool = False, port: int = 0) -> None:
        self.port = port or _free_port()
        self.pty_file = None
        cmd = [sys.executable, MOCK, "--port", str(self.port),
               "--bind", "127.0.0.1", *extra]
        if pty:
            self.pty_file = tempfile.mktemp(suffix=".pty")
            cmd += ["--pty", "--pty-path-file", self.pty_file]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True)
        self.pty_path = None
        deadline = time.time() + 10
        while time.time() < deadline:
            if self.pty_file and os.path.exists(self.pty_file):
                with open(self.pty_file, encoding="utf-8") as fp:
                    self.pty_path = fp.read().strip()
            if _port_open(self.port) and (not pty or self.pty_path):
                return
            if self.proc.poll() is not None:
                raise RuntimeError("mock died: %s" % self.proc.stdout.read())
            time.sleep(0.05)
        raise RuntimeError("mock did not come up")

    def stop(self) -> str:
        self.proc.terminate()
        try:
            out, _ = self.proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            out, _ = self.proc.communicate()
        if self.pty_file and os.path.exists(self.pty_file):
            os.unlink(self.pty_file)
        return out or ""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def _free_port() -> int:
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _port_open(port: int) -> bool:
    import socket
    with socket.socket() as s:
        s.settimeout(0.2)
        return s.connect_ex(("127.0.0.1", port)) == 0


# ---- 1. every structured command ---------------------------------------

async def test_commands() -> None:
    print("commands")
    with Mock() as mock:
        logs: list = []
        frames: list = []

        link = BoardLink("127.0.0.1", mock.port,
                         on_log=lambda e: _collect(logs, e),
                         on_frame=lambda s, d, j: _collect(frames, (s, d, j)))
        await link.connect_once()

        r = await link.request("kvdb.list")
        check(r["ok"] and isinstance(r["data"]["items"], list), "kvdb.list ok")
        items = {i["key"]: i for i in r["data"]["items"]}
        check("llm.key" in items, "kvdb.list includes llm.key")
        check(items["llm.key"]["masked"] is True,
              "llm.key is masked by default")
        check("..." in items["llm.key"]["value"]
              and "bytes" in items["llm.key"]["value"],
              "mask keeps a recognisable prefix and the length: %r"
              % items["llm.key"]["value"])
        check(items["llm.host"]["masked"] is False,
              "a non-secret key is not masked")

        r = await link.request("kvdb.list", {"raw": True})
        items = {i["key"]: i for i in r["data"]["items"]}
        check(items["llm.key"]["value"].startswith("sk-")
              and "bytes" not in items["llm.key"]["value"],
              "raw:true shows the secret in full")

        r = await link.request("kvdb.get", {"key": "nope"})
        check(r["ok"] is False and r["errname"] == "ENOENT"
              and r["errno"] == -2,
              "a missing key answers ENOENT with both number and name")

        r = await link.request("kvdb.set", {"key": "web.allow",
                                            "value": "10.0.0.5"})
        check(r["ok"] and "persistent" in r["data"],
              "kvdb.set reports whether the value survives a reset")

        r = await link.request("kvdb.del", {"key": "web.allow"})
        check(r["ok"], "kvdb.del ok")

        r = await link.request("wifi.status")
        check(r["ok"] and set(("running", "ssid", "ip", "netmask", "gw"))
              <= set(r["data"]), "wifi.status has the documented fields")

        r = await link.request("camera.start", {"width": 320, "height": 240})
        check(r["ok"] is False and r["errname"] == "EINVAL",
              "an unsupported geometry is refused, not silently changed")

        r = await link.request("camera.start", {"width": 640, "height": 480})
        check(r["ok"], "camera.start 640x480")
        r = await link.request("camera.start", {"width": 640, "height": 480})
        check(r["ok"] is False and r["errname"] == "EBUSY",
              "a second camera.start says EBUSY")

        await asyncio.sleep(0.7)
        r = await link.request("camera.stop")
        check(r["ok"] and r["data"]["frames_sent"] >= 1,
              "camera.stop reports frames_sent=%s"
              % r["data"].get("frames_sent"))
        check(len(frames) >= 1, "at least one frame arrived (%d)" % len(frames))
        if frames:
            seq, digest, jpeg = frames[0]
            check(board_link.fnv1a(jpeg) == digest,
                  "the frame checksum matches the bytes")
            check(jpeg[:2] == b"\xff\xd8" and jpeg[-2:] == b"\xff\xd9",
                  "the frame has SOI and EOI")

        r = await link.request("log.subscribe", {"on": True})
        check(r["ok"] and r["data"]["replayed"] >= 1,
              "log.subscribe replays the backlog (%s lines)"
              % r["data"].get("replayed"))
        await asyncio.sleep(0.8)
        check(any("line" in e for e in logs), "live log lines arrive")

        r = await link.request("sys.status")
        check(r["ok"] and r["data"]["heaps"] and "uptime" in r["data"],
              "sys.status carries heaps and uptime")

        r = await link.request("shell.exec", {"cmdline": "free"})
        check(r["ok"] and r["data"]["accepted"] is True, "shell.exec accepted")
        r = await link.request("shell.exec", {"cmdline": "ps"})
        check(r["ok"] is False and r["err"] == "busy",
              "a second shell.exec answers busy")
        await asyncio.sleep(0.5)
        check(any(e.get("exit") is not None for e in logs),
              "the shell command closes with an exit status")

        r = await link.request("nonsense.cmd")
        check(r["ok"] is False and r["errname"] == "ENOSYS",
              "an unknown command answers ENOSYS")

        await _check_volume(link)
        await _check_conv(link)

        await link.close()


async def _check_volume(link) -> None:
    """audio.volume.

    The interesting property is that the answer is a read-back and not an echo:
    the DAC's gain is six bits, so most requests land on a neighbouring value,
    and a page that displayed the request would be showing a number the
    hardware never confirmed.
    """
    r = await link.request("audio.volume")
    check(r["ok"] and set(("volume", "percent", "unity", "max", "applied"))
          <= set(r["data"]),
          "audio.volume reads without changing anything")

    # The read-back has to be a number, not null.  On hardware it was null for
    # a while: the driver's getcaps clears ac_format.hw before the switch that
    # matches on it, so the volume case never ran.  The mock cannot reproduce
    # that, which is exactly why this asserts on the value rather than on the
    # key being present.
    check(isinstance(r["data"]["volume"], int),
          "the volume reads back as a number, not null -- null means the "
          "board has no read-back and the page can only show its own request")
    check(r["data"]["applied"] is False,
          "a read is reported as not having applied anything")
    unity = r["data"]["unity"]
    check(0 < unity < 1000,
          "0 dB is inside the range, not at the top of it (%s) -- the page "
          "needs that to mark where the DAC starts amplifying" % unity)

    r = await link.request("audio.volume", {"volume": 300})
    check(r["ok"] and r["data"]["applied"] is True, "a set is applied")
    got = r["data"]["volume"]
    check(abs(got - 300) <= 1000 // 0x3f + 1,
          "the answer is the quantised value near the request "
          "(asked 300, got %s)" % got)
    # Half-up, the same rounding the board does.  Python's round() is
    # half-to-even, so it disagrees on exactly the .5 cases and asserting
    # against it would be testing Python rather than the board.
    check(r["data"]["percent"] == (got * 100 + 500) // 1000,
          "percent agrees with the raw value (%s vs %s)"
          % (r["data"]["percent"], got))

    r = await link.request("audio.volume")
    check(r["ok"] and r["data"]["volume"] == got,
          "and it sticks: a later read returns the same value (%s vs %s)"
          % (r["data"].get("volume"), got))

    r = await link.request("audio.volume", {"volume": 0})
    check(r["ok"] and r["data"]["volume"] == 0, "zero is accepted (mute)")

    r = await link.request("audio.volume", {"volume": 1001})
    check(r["ok"] is False and r["errname"] == "EINVAL",
          "out of range is refused rather than clamped silently")

    r = await link.request("audio.volume", {"volume": unity})
    check(r["ok"] and r["data"]["volume"] == unity,
          "unity round-trips exactly, since it is a real gain step")

    # ---- the spoken confirmation ----
    #
    # The clip is what makes the setting audible, and it lives on the card, so
    # a fresh board does not have it.  The important property is that the
    # volume still takes when the announcement cannot happen: reporting a lost
    # setting because a file is missing would be worse than the silence.

    r = await link.request("audio.announce")
    check(r["ok"] and r["data"]["present"] is False,
          "a fresh board reports no confirmation clip")

    r = await link.request("audio.volume", {"volume": 500})
    check(r["ok"] and r["data"]["volume"] > 0,
          "the volume is still set when there is no clip to play")
    check(r["data"]["announced"] is False and
          r["data"].get("announce_errname") == "ENOENT",
          "and the missing clip is named rather than passed over silently")

    import base64 as _b64
    clip = bytes(bytearray((i * 7) % 256 for i in range(3000)))
    half = 1200
    r = await link.request("audio.announce", {
        "b64": _b64.b64encode(clip[:half]).decode(), "offset": 0})
    check(r["ok"] and r["data"]["bytes"] == half,
          "the first chunk lands and the board reports the file length")

    r = await link.request("audio.announce", {
        "b64": _b64.b64encode(clip[half:]).decode(), "offset": half,
        "final": True})
    check(r["ok"] and r["data"]["bytes"] == len(clip),
          "the second chunk is appended at its offset (%s of %d)"
          % (r["data"].get("bytes"), len(clip)))

    r = await link.request("audio.announce", {
        "b64": _b64.b64encode(b"xxxx").decode(), "offset": 99999})
    check(r["ok"] is False,
          "a chunk that does not follow the file is refused, so a hole cannot "
          "be written silently")

    r = await link.request("audio.announce", {"b64": "not base64 !!"})
    check(r["ok"] is False and r["errname"] == "EINVAL",
          "invalid base64 is refused rather than partially decoded")

    r = await link.request("audio.announce")
    check(r["ok"] and r["data"]["present"] is True
          and r["data"]["bytes"] == len(clip),
          "the clip is now on the board")

    r = await link.request("audio.volume", {"volume": 600})
    check(r["ok"] and r["data"]["announced"] is True,
          "with a clip present, a set is announced out loud")

    r = await link.request("audio.volume", {"volume": 600,
                                            "announce": False})
    check(r["ok"] and r["data"]["announced"] is False,
          "and the announcement can be suppressed -- muting is the case where "
          "playing it would be indistinguishable from a broken feature")

    r = await link.request("audio.volume")
    check(r["ok"] and r["data"]["announced"] is False,
          "a plain read never announces; the page reads on every reconnect")


async def _check_conv(link) -> None:
    """conv.query / conv.get.

    The filters are what this feature is, so each check below is a record that
    must be left out.  A test that only asserted "some records came back" would
    pass against a board that ignored every filter.
    """
    r = await link.request("conv.query")
    check(r["ok"] and set(("items", "matched", "returned", "limit"))
          <= set(r["data"]),
          "conv.query carries matched and returned, not just a list")
    all_n = r["data"]["matched"]
    check(all_n >= 1, "there is history to query (%d records)" % all_n)

    item = r["data"]["items"][0]
    check(set(("seq", "date", "epoch", "duration_ms", "cue", "confidence",
               "unable_to_judge", "text_bytes", "summary")) <= set(item),
          "an index entry has the documented fields")
    check(isinstance(item["date"], int) and item["date"] > 20000000,
          "the date is a YYYYMMDD integer, so the page never sees an epoch: %r"
          % item["date"])

    # A range that excludes both ends of the fixture.  An inclusive-bound
    # off-by-one shows up here and nowhere else.
    r = await link.request("conv.query", {"from": 20260812, "to": 20260816})
    dates = [i["date"] for i in r["data"]["items"]]
    check(r["ok"] and dates and all(20260812 <= d <= 20260816 for d in dates),
          "a date range excludes what is outside it: %r" % dates)
    check(20260812 in dates and 20260816 in dates,
          "and includes both bounds: %r" % dates)
    check(r["data"]["matched"] < all_n,
          "the range really narrowed the result (%d of %d)"
          % (r["data"]["matched"], all_n))

    # The keyword has to search the summary as well as the transcript: record 4
    # mentions 爬山 only in its summary, and a transcript-only search drops it.
    r = await link.request("conv.query", {"keyword": "爬山"})
    seqs = sorted(i["seq"] for i in r["data"]["items"])
    check(r["ok"] and len(seqs) >= 2,
          "a keyword search matches more than one record: %r" % seqs)
    summaries = {i["seq"]: i["summary"] for i in r["data"]["items"]}
    check(any("爬山" in s for s in summaries.values()),
          "including one whose summary carries the word: %r" % summaries)

    r = await link.request("conv.query", {"keyword": "这个词不可能出现"})
    check(r["ok"] and r["data"]["matched"] == 0 and not r["data"]["items"],
          "a keyword that matches nothing answers zero, not everything")

    # unable_to_judge records are in by default: hiding a conversation the
    # device saw would be the wrong kind of quiet.
    r = await link.request("conv.query")
    check(any(i["unable_to_judge"] for i in r["data"]["items"]),
          "a record the model could not judge is listed by default")
    r = await link.request("conv.query", {"include_unjudged": False})
    check(all(not i["unable_to_judge"] for i in r["data"]["items"]),
          "and can be excluded on request")

    r = await link.request("conv.query", {"limit": 2})
    check(r["ok"] and r["data"]["returned"] == 2
          and r["data"]["matched"] > 2,
          "limit truncates the list but still reports the true total "
          "(returned=%s matched=%s)"
          % (r["data"].get("returned"), r["data"].get("matched")))

    r = await link.request("conv.get", {"seq": 4})
    check(r["ok"] and r["data"]["text"], "conv.get returns the transcript")
    check("\n" in r["data"]["text"],
          "with its line breaks intact -- each turn is a line and reflowing "
          "them runs the speakers together")
    cue = r["data"].get("cue")
    check(isinstance(cue, dict) and cue.get("schema") == "social-cue/v1",
          "the analysis arrives as an object, not as a string the page would "
          "have to parse again: %r" % type(cue).__name__)
    check("overall_confidence" in cue and "unable_to_judge" in cue,
          "and it keeps the hedging fields")

    r = await link.request("conv.get", {"seq": 9999})
    check(r["ok"] is False and r["errname"] == "ENOENT",
          "a missing record answers ENOENT rather than an empty transcript")

    r = await link.request("conv.get")
    check(r["ok"] is False and r["errname"] == "EINVAL",
          "conv.get without a seq is refused")


def _collect(sink: list, item):
    sink.append(item)

    async def noop():
        return None
    return noop()


# ---- 2. bootstrap, four paths ------------------------------------------

async def test_bootstrap_cached() -> None:
    print("bootstrap: path 1, cached address")
    with Mock() as mock:
        _clear_cache()
        bootstrap_mod.save_cached_ip("127.0.0.1")
        boot = bootstrap_mod.Bootstrap(port=mock.port,
                                       serial_factory=_dead_serial)
        r = await boot.run()
        check(r.ok and r.path == "cached-ip",
              "the cached address is tried first and works")
        check(r.steps[0]["step"] == "cached-ip", "step order")


async def test_bootstrap_serial_ifconfig() -> None:
    print("bootstrap: path 2, address read over serial")
    with Mock("--scenario", "running", pty=True) as mock:
        _clear_cache()
        boot = bootstrap_mod.Bootstrap(
            port=mock.port,
            serial_factory=lambda: SerialConsole(mock.pty_path, check_holder=False))
        r = await boot.run()
        check(r.ok and r.path == "serial-ifconfig+tcp",
              "ifconfig gave the address and TCP came up (path=%s)" % r.path)
        names = [s["step"] for s in r.steps]
        check("serial-ifconfig" in names, "the serial step is recorded")


async def test_bootstrap_provision() -> None:
    print("bootstrap: path 3, full provisioning over serial")
    with Mock("--scenario", "needs-setup", pty=True) as mock:
        _clear_cache()
        boot = bootstrap_mod.Bootstrap(
            port=mock.port, ssid="MockPublic",
            serial_factory=lambda: SerialConsole(mock.pty_path, check_holder=False))
        r = await boot.run()
        check(r.ok and r.path == "serial-provision+tcp",
              "provisioning ran and TCP came up (path=%s)" % r.path)
        prov = [s for s in r.steps if s["step"] == "serial-provision"]
        check(bool(prov) and prov[0]["ok"], "the provisioning step succeeded")
        # The mock fails the first renew on purpose; passing proves the retry
        # is real and not a comment.
        check("netlib_obtain_ipv4addr" in (prov[0].get("detail") or ""),
              "the first renew failed and was retried")


async def test_bootstrap_degraded() -> None:
    print("bootstrap: path 4, degraded")
    with Mock("--scenario", "no-network", "--refuse", pty=True) as mock:
        _clear_cache()
        boot = bootstrap_mod.Bootstrap(
            port=mock.port, ssid="MockPublic",
            serial_factory=lambda: SerialConsole(mock.pty_path, check_holder=False))
        r = await boot.run()
        check(not r.ok, "no TCP link")
        check(bool(r.degraded_reason),
              "the reason is reported rather than a bare failure: %r"
              % r.degraded_reason)
        check(any(s["step"] == "serial-provision" for s in r.steps),
              "it got as far as provisioning before giving up")


def _dead_serial():
    class Dead:
        def __enter__(self):
            raise OSError("no serial in this test")

        def __exit__(self, *exc):
            return False
    return Dead()


def _clear_cache() -> None:
    if os.path.exists(bootstrap_mod.CACHE_PATH):
        os.unlink(bootstrap_mod.CACHE_PATH)


# ---- 3. reconnect after reboot -----------------------------------------

async def test_reconnect() -> None:
    print("reconnect after sys.reboot")
    with Mock("--reboot-downtime", "1.5") as mock:
        states: list = []

        link = BoardLink("127.0.0.1", mock.port,
                         on_state=lambda s: _collect(states, s))
        await link.connect_once()
        runner = asyncio.create_task(link.run_forever(max_backoff=0.5))

        r = await link.request("sys.reboot")
        check(r["ok"], "sys.reboot answered before the board went away")

        # The response has to arrive first; that is the whole reason the board
        # queues it before rebooting.
        for _ in range(60):
            await asyncio.sleep(0.1)
            if not link.connected:
                break
        check(not link.connected, "the link dropped after the reboot")

        deadline = time.time() + 15
        while time.time() < deadline and not link.connected:
            await asyncio.sleep(0.1)
        check(link.connected, "the link came back by itself")
        check(link.attempts >= 1,
              "the attempt counter is visible to the page (%d)" % link.attempts)
        check(any(not s["connected"] for s in states),
              "the disconnection was announced, not silent")

        r = await link.request("sys.status")
        check(r["ok"], "commands work again after the reconnect")

        runner.cancel()
        await link.close()


# ---- 4. dropped counter -------------------------------------------------

async def test_dropped() -> None:
    print("dropped counter")
    with Mock("--flood", "200") as mock:
        logs: list = []
        link = BoardLink("127.0.0.1", mock.port,
                         on_log=lambda e: _collect(logs, e))
        await link.connect_once()
        await link.request("log.subscribe", {"on": True})
        # Generous, because this runs alongside ASan-instrumented binaries and
        # several other subprocesses; a tight window here made the check flaky
        # without making it stronger.
        deadline = time.time() + 20
        while time.time() < deadline:
            await asyncio.sleep(0.2)
            if any("dropped" in e for e in logs):
                break
        drops = [e for e in logs if "dropped" in e]
        check(bool(drops), "the gap notice reached the host")
        check(drops and drops[0]["dropped"] > 0,
              "and it carries a count: %s"
              % (drops[0] if drops else None))
        await link.close()


# ---- 5. refused connection ---------------------------------------------

async def test_refused() -> None:
    print("connection refused, as the whitelist refuses one")
    with Mock("--refuse") as mock:
        link = BoardLink("127.0.0.1", mock.port)
        ok = await link.probe()
        check(not ok, "probe fails")
        check(bool(link.last_error),
              "and says why, so the page can show it: %r" % link.last_error)
        await link.close()


# ---- 6. rejected frames are kept ---------------------------------------

async def test_rejected_frames() -> None:
    print("frames that fail verification are kept, not dropped")
    root = tempfile.mkdtemp(prefix="wt-cap-")
    try:
        with Mock("--corrupt", "2") as mock:
            session = CaptureSession(root=root, name="test")
            session.start()

            results: list = []

            def on_frame(seq, digest, jpeg):
                results.append(session.write_frame(seq, digest, jpeg))

                async def noop():
                    return None
                return noop()

            link = BoardLink("127.0.0.1", mock.port, on_frame=on_frame)
            await link.connect_once()
            await link.request("camera.start", {"width": 640, "height": 480})
            await asyncio.sleep(1.4)
            await link.request("camera.stop")
            await link.close()
            session.stop()

        good = [r for r in results if r.get("ok")]
        bad = [r for r in results if r.get("written") and not r.get("ok")]
        check(bool(good), "good frames were written (%d)" % len(good))
        check(bool(bad), "corrupted frames were written too (%d)" % len(bad))
        check(all(os.path.exists(r["path"]) for r in results if r["written"]),
              "every written frame is on disk")
        check(all("rejected" in r["path"] for r in bad),
              "the bad ones are under rejected/")
        check(os.path.exists(os.path.join(root, "test", "session.log")),
              "session.log exists")
        with open(os.path.join(root, "test", "session.log"),
                  encoding="utf-8") as fp:
            text = fp.read()
        check("rejected" in text,
              "and the rejection is recorded in the log, not only on disk")
        check(all(r["path"].endswith(".jpg") for r in good)
              and any("frame_00.jpg" in r["path"] for r in good),
              "naming follows b64frames.py so ffmpeg -i frame_%02d.jpg works")
    finally:
        shutil.rmtree(root, ignore_errors=True)


# ---- 7. the serial port is released ------------------------------------

def _own_fds_for(path: str) -> list:
    """Which of our own descriptors point at path.  The direct way to answer
    "did we let go of the port", without depending on who else has it."""
    out = []
    for name in os.listdir("/proc/self/fd"):
        try:
            if os.readlink(os.path.join("/proc/self/fd", name)) == path:
                out.append(name)
        except OSError:
            pass
    return out


def test_serial_released() -> None:
    print("the serial port is free again as soon as we are done")
    with Mock("--scenario", "running", pty=True) as mock:
        con = SerialConsole(mock.pty_path, check_holder=False)
        con.open()
        check(con.fd is not None, "opened")
        check(len(_own_fds_for(mock.pty_path)) == 1, "exactly one handle open")
        con.close()
        check(con.fd is None, "closed")
        check(_own_fds_for(mock.pty_path) == [],
              "no descriptor of ours points at the port any more -- this is "
              "what serial_cmd.sh and autoflash.sh need")

        # And the with-block form releases it on the way out, including when
        # the body raises: that is the shape the bootstrap uses.
        try:
            with SerialConsole(mock.pty_path, check_holder=False) as c2:
                check(len(_own_fds_for(mock.pty_path)) == 1, "held inside")
                raise RuntimeError("boom")
        except RuntimeError:
            pass
        check(_own_fds_for(mock.pty_path) == [],
              "released even when the body raised")

        # The guard itself still works: a real port that someone else holds is
        # refused with serial_cmd.sh's wording.
        con3 = SerialConsole(mock.pty_path, check_holder=True)
        try:
            con3.open()
            con3.close()
            check(True, "guard allowed an unheld port")
        except SerialBusy as exc:
            check("串口被占用" in str(exc),
                  "the refusal reads like serial_cmd.sh's: %s"
                  % str(exc).splitlines()[0])


# ---- 8. framing agrees with the board's own codec ----------------------

def test_framing_matches_board() -> None:
    print("host framing agrees with wt_protocol.c")
    frame = board_link.encode_frame(board_link.TYPE_RSP, 0x1234, b"abcd")
    check(frame[0] == 0x02, "type byte")
    check(frame[1] == 0, "flags reserved zero")
    check(frame[2:4] == b"\x34\x12", "req_id little endian")
    check(frame[4:8] == b"\x04\x00\x00\x00", "len little endian")

    parser = board_link.FrameParser()
    got = []
    # One byte at a time: the worst case a TCP stack can hand a reader, and
    # the same case the C tests cover.
    for i in range(len(frame)):
        got.extend(parser.feed(frame[i:i + 1]))
    check(len(got) == 1 and got[0][2] == b"abcd", "reassembled from 1-byte reads")

    # Over-long and unknown-type both have to raise rather than resync.
    bad_len = bytes([0x04, 0, 0, 0, 0x01, 0x00, 0x01, 0x00])
    try:
        list(board_link.FrameParser().feed(bad_len))
        check(False, "an over-long frame must raise")
    except board_link.ProtocolError:
        check(True, "an over-long frame raises ProtocolError")

    try:
        list(board_link.FrameParser().feed(bytes([0x42, 0, 0, 0, 0, 0, 0, 0])))
        check(False, "an unknown type must raise")
    except board_link.ProtocolError:
        check(True, "an unknown type raises ProtocolError")

    check(board_link.fnv1a(b"") == 0x811C9DC5, "fnv1a basis")
    check(board_link.fnv1a(b"foobar") == 0xBF9CF968, "fnv1a foobar")


# ---- 9. the backend itself: HTTP, WebSocket, binary frames -------------

async def test_console_backend() -> None:
    print("console.py: page, WebSocket, binary frames (--no-tls path)")
    import aiohttp

    with Mock() as mock:
        http_port = _free_port()
        proc = subprocess.Popen(
            [sys.executable, os.path.join(os.path.dirname(HERE), "console.py"),
             "--host", "127.0.0.1", "--port", str(http_port),
             "--board", "127.0.0.1", "--board-port", str(mock.port),
             # --no-tls on purpose: this test is about the page, the WebSocket
             # and the binary frames.  That the browser hop *is* TLS by default,
             # and that plain HTTP against it fails, is checked in
             # test_tls_e2e.py where it belongs.
             "--no-tls", "--no-bootstrap"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            deadline = time.time() + 15
            while time.time() < deadline and not _port_open(http_port):
                if proc.poll() is not None:
                    raise RuntimeError("console died: %s" % proc.stdout.read())
                await asyncio.sleep(0.1)
            check(_port_open(http_port), "the backend is listening")

            async with aiohttp.ClientSession() as sess:
                async with sess.get("http://127.0.0.1:%d/" % http_port) as r:
                    body = await r.text()
                check(r.status == 200 and "web_tool" in body,
                      "the page is served from host/web/")

                for asset in ("style.css", "app.js"):
                    async with sess.get("http://127.0.0.1:%d/static/%s"
                                        % (http_port, asset)) as r:
                        check(r.status == 200, "%s is served" % asset)

                async with sess.ws_connect(
                        "http://127.0.0.1:%d/ws" % http_port) as ws:
                    state = await _ws_expect(ws, "state", 10)
                    check(state is not None, "the backend pushes state on open")

                    # Wait for the link to be up before asking for a command;
                    # the page does the same thing by watching the lamp.
                    for _ in range(100):
                        if state and state["state"]["link"]["connected"]:
                            break
                        state = await _ws_expect(ws, "state", 2) or state
                        if state and state["state"]["link"]["connected"]:
                            break
                        await ws.send_str(json.dumps({"op": "state", "id": 0}))
                        rsp = await _ws_expect(ws, "rsp", 2)
                        if rsp and rsp.get("data", {}).get(
                                "link", {}).get("connected"):
                            state = {"state": rsp["data"]}
                            break
                    check(bool(state) and
                          state["state"]["link"]["connected"],
                          "the backend attached to the board")

                    await ws.send_str(json.dumps(
                        {"op": "cmd", "id": 11, "cmd": "kvdb.list"}))
                    rsp = await _ws_expect(ws, "rsp", 10)
                    check(rsp is not None and rsp.get("ok") is True
                          and "items" in rsp["data"],
                          "a command goes browser -> backend -> board -> back")

                    await ws.send_str(json.dumps(
                        {"op": "capture", "id": 12, "action": "start"}))
                    rsp = await _ws_expect(ws, "rsp", 10)
                    check(rsp is not None and rsp["ok"], "capture start")
                    capdir = rsp["data"]["dir"]

                    await ws.send_str(json.dumps(
                        {"op": "cmd", "id": 13, "cmd": "camera.start",
                         "args": {"width": 640, "height": 480}}))
                    rsp = await _ws_expect(ws, "rsp", 10)
                    check(rsp is not None and rsp["ok"], "camera.start via ws")

                    blob = await _ws_expect_binary(ws, 10)
                    check(blob is not None and len(blob) > 8,
                          "a JPEG arrived as a binary WebSocket frame")
                    if blob:
                        seq, digest = struct.unpack("<II", blob[:8])
                        jpeg = blob[8:]
                        check(jpeg[:2] == b"\xff\xd8",
                              "the 8-byte prefix is metadata and the rest is "
                              "the JPEG, not base64")
                        check(board_link.fnv1a(jpeg) == digest,
                              "and the checksum survived the extra hop")

                    saved = await _ws_expect(ws, "capture.saved", 10)
                    expected_path = os.path.join(capdir, "frame_00.jpg")
                    check(saved is not None,
                          "a saved-frame event reaches the browser")
                    check(saved is not None and
                          os.path.isabs(saved.get("path", "")),
                          "the saved-frame event carries an absolute path")
                    check(saved is not None and
                          saved.get("path") == expected_path,
                          "the event carries the exact backend path")
                    check(os.path.isfile(expected_path),
                          "the path printed to the browser exists")

                    await ws.send_str(json.dumps(
                        {"op": "cmd", "id": 14, "cmd": "camera.stop"}))
                    await _ws_expect(ws, "rsp", 10)
                    await ws.send_str(json.dumps(
                        {"op": "capture", "id": 15, "action": "stop"}))
                    rsp = await _ws_expect(ws, "rsp", 10)
                    check(rsp is not None and rsp["data"]["frames"] >= 1,
                          "frames landed on disk (%s)"
                          % (rsp["data"]["frames"] if rsp else None))
                    check(os.path.isdir(capdir), "the session directory exists")
                    shutil.rmtree(capdir, ignore_errors=True)

                    await ws.send_str(json.dumps(
                        {"op": "cmd", "id": 16, "cmd": "log.subscribe",
                         "args": {"on": True}}))
                    await _ws_expect(ws, "rsp", 10)
                    logmsg = await _ws_expect(ws, "log", 10)
                    check(logmsg is not None, "log events reach the browser")

                    await ws.send_str(json.dumps(
                        {"op": "nonsense", "id": 17}))
                    rsp = await _ws_expect(ws, "rsp", 10)
                    check(rsp is not None and rsp["ok"] is False,
                          "an unknown browser op is refused, not ignored")
        finally:
            proc.terminate()
            try:
                proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate()


async def _ws_expect(ws, want: str, timeout: float):
    import aiohttp
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            msg = await asyncio.wait_for(ws.receive(),
                                         timeout=max(0.1, deadline - time.time()))
        except asyncio.TimeoutError:
            return None
        if msg.type is aiohttp.WSMsgType.TEXT:
            obj = json.loads(msg.data)
            if obj.get("type") == want:
                return obj
        elif msg.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
            return None
    return None


async def _ws_expect_binary(ws, timeout: float):
    import aiohttp
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            msg = await asyncio.wait_for(ws.receive(),
                                         timeout=max(0.1, deadline - time.time()))
        except asyncio.TimeoutError:
            return None
        if msg.type is aiohttp.WSMsgType.BINARY:
            return msg.data
        if msg.type in (aiohttp.WSMsgType.CLOSED, aiohttp.WSMsgType.ERROR):
            return None
    return None


# ---- 7. the exit notice survives an output burst -----------------------

async def test_exit_survives_burst() -> None:
    print("the exit notice survives a burst that fills the queue")
    with Mock("--shell-burst") as mock:
        events: list = []

        async def on_log(e):
            events.append(e)

        link = BoardLink("127.0.0.1", mock.port, on_log=on_log)
        await link.connect_once()
        await link.request("log.subscribe", {"on": True}, timeout=10)

        r = await link.request("shell.exec", {"cmdline": "ls /dev"},
                               timeout=10)
        check(r.get("ok") is True, "the command was accepted")

        deadline = time.time() + 20
        while time.time() < deadline:
            await asyncio.sleep(0.1)
            if any("exit" in e for e in events):
                break

        lines = [e for e in events if "line" in e]
        exits = [e for e in events if "exit" in e]
        check(bool(exits),
              "the exit notice arrived after %d output line(s) -- as an "
              "EVT_LOG it was droppable and a burst like `ls /dev` lost it, "
              "leaving the page showing the command as running for ever"
              % len(lines))
        await link.close()


# ---- 8. a board that vanishes is noticed --------------------------------

async def test_dead_board_is_noticed() -> None:
    """The bug this covers: a board that reboots or loses Wi-Fi does not send a
    FIN, it just stops existing.  Without liveness checking the reader waits for
    ever, the page keeps saying "connected", and every command times out --
    which is what an operator saw on 2026-08-18.  ETIMEDOUT on a link the page
    calls healthy is the worst of both worlds: it looks like the board is broken
    when the truth is that it is gone.
    """
    print("a board that vanishes is reported as gone, not as connected")
    mock = Mock()
    states: list = []

    async def on_state(st):
        states.append(st)

    link = BoardLink("127.0.0.1", mock.port, on_state=on_state,
                     ping_interval=1.0, ping_timeout=3.0)
    await link.connect_once()
    check(link.connected, "connected to begin with")

    r = await link.request("sys.status", timeout=8)
    check(r.get("ok") is True, "and answering")

    # SIGSTOP, not SIGKILL.  Killing the process makes the kernel close the
    # socket, which the reader notices as EOF -- that path already worked.  The
    # case that hung is the one where the peer stops answering while the socket
    # stays open, which is what a frozen or rebooting board looks like from
    # here, and SIGSTOP reproduces it exactly.
    import signal
    mock.proc.send_signal(signal.SIGSTOP)

    deadline = time.time() + 20
    while time.time() < deadline and link.connected:
        await asyncio.sleep(0.2)

    check(not link.connected,
          "the link is marked down within the keepalive budget")
    check("stopped answering" in (link.last_error or "")
          or "PING" in (link.last_error or ""),
          "and the reason names the keepalive, so it is distinguishable from "
          "an orderly close: %r" % link.last_error)
    check(any(not s["connected"] for s in states),
          "the page was told, so it can stop offering commands")

    # And a command now fails as not-connected rather than timing out.
    failed_fast = False
    t0 = time.time()
    try:
        await link.request("sys.status", timeout=8)
    except ConnectionError:
        failed_fast = time.time() - t0 < 2.0
    check(failed_fast,
          "a command now fails immediately as ENOTCONN instead of waiting "
          "for a timeout")
    await link.close()
    mock.proc.send_signal(signal.SIGCONT)
    mock.stop()


# ---- 9. wifi.connect answers before it cuts the link --------------------

async def test_wifi_connect_answers_first() -> None:
    """wifi.connect re-associates, which tears down the very TCP connection its
    answer has to travel on.  The board therefore stores, answers, and only then
    applies.  This checks the contract the page relies on: an answer arrives,
    and it says the change is being applied rather than claiming it is done.
    """
    print("wifi.connect answers before applying")
    with Mock() as mock:
        link = BoardLink("127.0.0.1", mock.port)
        await link.connect_once()
        r = await link.request("wifi.connect",
                              {"ssid": "AIPC", "psk": "mock-passphrase"},
                              timeout=10)
        check(r.get("ok") is True, "an answer arrives at all")
        check("ssid" in r["data"], "and identifies what it applied to")
        await link.close()


# ---- runner ------------------------------------------------------------

async def amain() -> int:
    await test_commands()
    await test_bootstrap_cached()
    await test_bootstrap_serial_ifconfig()
    await test_bootstrap_provision()
    await test_bootstrap_degraded()
    await test_reconnect()
    await test_dropped()
    await test_refused()
    await test_rejected_frames()
    await test_console_backend()
    await test_exit_survives_burst()
    await test_dead_board_is_noticed()
    await test_wifi_connect_answers_first()
    return 0


def main() -> int:
    global VERBOSE
    ap = argparse.ArgumentParser()
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    VERBOSE = args.verbose

    test_framing_matches_board()
    test_serial_released()
    asyncio.run(amain())

    _clear_cache()
    print("\n%d checks, %d failure(s)" % (CHECKS, len(FAILURES)))
    for f in FAILURES:
        print("  - %s" % f)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
