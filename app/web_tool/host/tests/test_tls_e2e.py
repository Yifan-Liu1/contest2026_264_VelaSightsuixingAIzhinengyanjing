#!/usr/bin/env python3
"""End-to-end tests for the TLS path: the board dials in, both ends check.

This covers the arrangement the hardware forced on us.  The board is behind the
access point's NAT (measured 2026-08-18: outbound 91 ms, inbound never), so it
dials the console instead of listening, and the two ends authenticate in the
only ways they each can:

    console -> board   its TLS certificate, pinned by SHA-256 (kvdb web.fp)
    board -> console   the shared token in the HELLO frame (kvdb web.token)

Both halves are tested for the failure direction as well as the success one,
because a check that cannot be observed to reject anything is not a check.

Run: ./test_tls_e2e.py
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import os
import ssl
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import tlsconf                                          # noqa: E402
from board_link import BoardServer                      # noqa: E402

MOCK = os.path.join(os.path.dirname(HERE), "mock_board.py")
CONSOLE = os.path.join(os.path.dirname(HERE), "console.py")

CHECKS = 0
FAILURES: list[str] = []


def check(cond: bool, what: str) -> bool:
    global CHECKS
    CHECKS += 1
    if cond:
        return True
    FAILURES.append(what)
    print("  FAIL %s" % what)
    return False


def free_port() -> int:
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def port_open(port: int, host: str = "127.0.0.1") -> bool:
    import socket
    with socket.socket() as s:
        s.settimeout(0.2)
        return s.connect_ex((host, port)) == 0


class Mock:
    """mock_board.py in outbound TLS mode."""

    def __init__(self, *extra: str) -> None:
        self.proc = subprocess.Popen(
            [sys.executable, MOCK, *extra],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    def stop(self) -> str:
        self.proc.terminate()
        try:
            out, _ = self.proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            out, _ = self.proc.communicate()
        return out or ""

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


# ---- 1. certificate material -------------------------------------------

def test_cert_material() -> None:
    print("certificate and fingerprint")
    cert, key = tlsconf.ensure_cert()
    check(os.path.exists(cert) and os.path.exists(key),
          "certificate and key exist")
    check(oct(os.stat(key).st_mode)[-3:] == "600",
          "the private key is not world readable (%s)"
          % oct(os.stat(key).st_mode)[-3:])

    fp = tlsconf.fingerprint(cert)
    check(len(fp) == 64 and all(c in "0123456789abcdef" for c in fp),
          "fingerprint is lowercase hex sha256: %s" % fp)

    # The same value openssl prints, which is what makes it comparable by eye
    # with what the board reports.
    out = subprocess.run(
        ["bash", "-c",
         "openssl x509 -in %s -outform der | sha256sum" % cert],
        capture_output=True, text=True)
    check(out.stdout.split()[0] == fp,
          "openssl agrees with tlsconf.fingerprint()")

    # Reusing the file matters: the operator pins this value on the board, and
    # regenerating it on every start would silently break the pin.
    fp2 = tlsconf.fingerprint(tlsconf.ensure_cert()[0])
    check(fp2 == fp, "the fingerprint is stable across calls")

    # EC, not RSA: RSA keygen does not finish on the board, and ECDHE-ECDSA is
    # the cheap handshake there.
    text = subprocess.run(["openssl", "x509", "-in", cert, "-noout", "-text"],
                          capture_output=True, text=True).stdout
    check("id-ecPublicKey" in text and "P-256" in text,
          "the key is EC P-256")


# ---- 2. the board dials in and is accepted -----------------------------

async def test_inbound_accepted() -> None:
    print("board dials in over TLS with the right token")
    port = free_port()
    token = "t0k3n-" + os.urandom(4).hex()
    cert, key = tlsconf.ensure_cert()
    fp = tlsconf.fingerprint(cert)

    logs: list = []
    frames: list = []
    states: list = []

    async def on_log(e):
        logs.append(e)

    async def on_frame(seq, digest, jpeg):
        frames.append((seq, digest, jpeg))

    async def on_state(st):
        states.append(st)

    server = BoardServer(token=token, port=port,
                         ssl_context=tlsconf.board_context(cert, key),
                         bind="127.0.0.1", on_log=on_log, on_frame=on_frame,
                         on_state=on_state)
    await server.start()

    with Mock("--connect", "127.0.0.1:%d" % port, "--token", token,
              "--pin-fp", fp, "--once"):
        deadline = time.time() + 20
        while time.time() < deadline and server.link is None:
            await asyncio.sleep(0.1)

        check(server.link is not None, "the board connected")
        check(server.accepted == 1, "accepted once (%d)" % server.accepted)
        check(server.rejected == 0, "nothing rejected")

        hello = server.link.hello if server.link else None
        check(hello is not None and hello.get("token") == token,
              "the hello carried the token")

        r = await server.request("sys.status", timeout=10)
        check(r.get("ok") is True, "a command works over TLS")

        r = await server.request("kvdb.list", timeout=10)
        items = {i["key"]: i for i in r["data"]["items"]}
        check(items["llm.key"]["masked"] is True,
              "the API key is still masked over the encrypted link -- "
              "encryption is not a reason to stop masking, the operator's "
              "screen and the capture files are the other exposure")

        r = await server.request("camera.start",
                                 {"width": 640, "height": 480}, timeout=10)
        check(r.get("ok") is True, "camera.start over TLS")
        await asyncio.sleep(0.8)
        await server.request("camera.stop", timeout=10)
        check(len(frames) >= 1,
              "JPEG frames cross the TLS link (%d)" % len(frames))
        if frames:
            import board_link
            seq, digest, jpeg = frames[0]
            check(board_link.fnv1a(jpeg) == digest,
                  "and the checksum survives the record layer, which is where "
                  "a 27 KB frame is split across several 16 KB TLS records")

        await server.close()


# ---- 3. wrong token is refused -----------------------------------------

async def test_wrong_token_refused() -> None:
    print("board with the wrong token is refused")
    port = free_port()
    cert, key = tlsconf.ensure_cert()
    fp = tlsconf.fingerprint(cert)

    server = BoardServer(token="the-real-token", port=port,
                         ssl_context=tlsconf.board_context(cert, key),
                         bind="127.0.0.1", hello_timeout=3.0)
    await server.start()

    with Mock("--connect", "127.0.0.1:%d" % port, "--token", "wrong",
              "--pin-fp", fp, "--once"):
        deadline = time.time() + 20
        while time.time() < deadline and server.rejected == 0:
            await asyncio.sleep(0.1)

        check(server.rejected >= 1,
              "the connection was rejected (%d)" % server.rejected)
        check(server.link is None or not server.link.connected,
              "and no session was published")
        check("token" in (server.last_reject or ""),
              "the reason names the token: %r" % server.last_reject)
        await server.close()


async def test_no_hello_refused() -> None:
    print("a peer that never says hello is dropped")
    port = free_port()
    cert, key = tlsconf.ensure_cert()

    server = BoardServer(token="tok", port=port,
                         ssl_context=tlsconf.board_context(cert, key),
                         bind="127.0.0.1", hello_timeout=1.0)
    await server.start()

    # A bare TLS client that completes the handshake and then says nothing --
    # which is what an unauthorised peer that found the port would look like.
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    reader, writer = await asyncio.open_connection("127.0.0.1", port, ssl=ctx)

    deadline = time.time() + 10
    while time.time() < deadline and server.rejected == 0:
        await asyncio.sleep(0.1)

    check(server.rejected >= 1, "silence is rejected")
    check("hello" in (server.last_reject or ""),
          "the reason says so: %r" % server.last_reject)
    writer.close()
    await server.close()


# ---- 4. the pin protects the board -------------------------------------

async def test_pin_mismatch_stops_board() -> None:
    print("a mismatched pin stops the board from talking")
    port = free_port()
    cert, key = tlsconf.ensure_cert()

    server = BoardServer(token="tok", port=port,
                         ssl_context=tlsconf.board_context(cert, key),
                         bind="127.0.0.1", hello_timeout=2.0)
    await server.start()

    bogus = "00" * 32
    with Mock("--connect", "127.0.0.1:%d" % port, "--token", "tok",
              "--pin-fp", bogus, "--once") as mock:
        await asyncio.sleep(3.0)
        check(server.link is None or not server.link.connected,
              "no session was established")
        out = mock.stop()
        check("mismatch" in out,
              "and the board side says why: %r" % out.strip()[-160:])
    await server.close()


# ---- 5. the browser hop is HTTPS + WSS ---------------------------------

async def test_browser_https() -> None:
    print("console.py serves HTTPS and WSS")
    import aiohttp

    https_port = free_port()
    board_port = free_port()
    proc = subprocess.Popen(
        [sys.executable, CONSOLE, "--host", "127.0.0.1",
         "--port", str(https_port), "--board-listen", "127.0.0.1",
         "--board-listen-port", str(board_port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(https_port):
            if proc.poll() is not None:
                raise RuntimeError("console died: %s" % proc.stdout.read())
            await asyncio.sleep(0.1)
        check(port_open(https_port), "the HTTPS port is open")
        check(port_open(board_port), "the board port is open")

        # Plain HTTP against the TLS port must not work: that is the check that
        # TLS is actually required and not merely available.
        plain_failed = False
        try:
            async with aiohttp.ClientSession() as sess:
                async with sess.get("http://127.0.0.1:%d/" % https_port,
                                    timeout=aiohttp.ClientTimeout(total=5)):
                    pass
        except Exception:                              # noqa: BLE001
            plain_failed = True
        check(plain_failed, "plain HTTP to the HTTPS port fails")

        # And HTTPS works, with the certificate we generated.
        ctx = ssl.create_default_context(cafile=tlsconf.CERT_PATH)
        ctx.check_hostname = False
        conn = aiohttp.TCPConnector(ssl=ctx)
        async with aiohttp.ClientSession(connector=conn) as sess:
            url = "https://127.0.0.1:%d/" % https_port
            async with sess.get(url) as r:
                body = await r.text()
            check(r.status == 200 and "web_tool" in body,
                  "the page is served over HTTPS")
            check("keystore.mjs" in body or "app.js" in body,
                  "and references the module the page needs")

            async with sess.get("https://127.0.0.1:%d/static/keystore.mjs"
                                % https_port) as r:
                ks = await r.text()
            check(r.status == 200 and "MAX_ATTEMPTS" in ks,
                  "keystore.mjs is served")

            async with sess.ws_connect("https://127.0.0.1:%d/ws"
                                       % https_port) as ws:
                msg = await asyncio.wait_for(ws.receive(), timeout=10)
                obj = json.loads(msg.data)
                check(obj.get("type") == "state", "WSS pushes state on open")
                st = obj["state"]
                check(st["tls"]["fingerprint"] == tlsconf.fingerprint(),
                      "the page is told the fingerprint to pin")
                check(any("web.fp" in c for c in st["pairing"]["commands"]),
                      "and the exact kvdb commands to type")
                check(any("web.token" in c for c in st["pairing"]["commands"]),
                      "including the token")

                # With no board connected, commands must fail loudly rather
                # than appear to work.
                await ws.send_str(json.dumps(
                    {"op": "cmd", "id": 1, "cmd": "sys.status"}))
                rsp = None
                for _ in range(40):
                    m = await asyncio.wait_for(ws.receive(), timeout=5)
                    if m.type is aiohttp.WSMsgType.TEXT:
                        o = json.loads(m.data)
                        if o.get("type") == "rsp" and o.get("id") == 1:
                            rsp = o
                            break
                check(rsp is not None and rsp.get("ok") is False
                      and rsp.get("errname") == "ENOTCONN",
                      "a command with no board answers ENOTCONN")
    finally:
        proc.terminate()
        try:
            proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()


# ---- 6. the whole chain: browser -> console -> board -------------------

async def test_full_chain() -> None:
    print("browser -> console -> board, end to end, both hops TLS")
    import aiohttp

    https_port = free_port()
    board_port = free_port()
    cert = tlsconf.ensure_cert()[0]
    fp = tlsconf.fingerprint(cert)

    proc = subprocess.Popen(
        [sys.executable, CONSOLE, "--host", "127.0.0.1",
         "--port", str(https_port), "--board-listen", "127.0.0.1",
         "--board-listen-port", str(board_port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    mock = None
    try:
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(board_port):
            await asyncio.sleep(0.1)

        token_path = os.path.join(tlsconf.TLS_DIR, "board-token")
        with open(token_path, encoding="utf-8") as fp_:
            token = fp_.read().strip()

        mock = Mock("--connect", "127.0.0.1:%d" % board_port,
                    "--token", token, "--pin-fp", fp)

        ctx = ssl.create_default_context(cafile=cert)
        ctx.check_hostname = False
        conn = aiohttp.TCPConnector(ssl=ctx)
        async with aiohttp.ClientSession(connector=conn) as sess:
            async with sess.ws_connect("https://127.0.0.1:%d/ws"
                                       % https_port) as ws:
                # Wait for the board to be reported connected.
                connected = False
                deadline = time.time() + 25
                while time.time() < deadline and not connected:
                    await ws.send_str(json.dumps({"op": "state", "id": 99}))
                    m = await asyncio.wait_for(ws.receive(), timeout=5)
                    if m.type is aiohttp.WSMsgType.TEXT:
                        o = json.loads(m.data)
                        if o.get("type") == "rsp" and o.get("id") == 99:
                            connected = o["data"]["link"]["connected"]
                    await asyncio.sleep(0.3)
                check(connected, "the page sees the board as connected")

                async def call(cid, name, args=None):
                    await ws.send_str(json.dumps(
                        {"op": "cmd", "id": cid, "cmd": name,
                         "args": args or {}}))
                    for _ in range(80):
                        m = await asyncio.wait_for(ws.receive(), timeout=8)
                        if m.type is aiohttp.WSMsgType.TEXT:
                            o = json.loads(m.data)
                            if o.get("type") == "rsp" and o.get("id") == cid:
                                return o
                    return None

                r = await call(1, "kvdb.set",
                               {"key": "llm.key", "value": "sk-e2e-secret"})
                check(r is not None and r.get("ok"),
                      "writing the LLM key reaches the board through both hops")

                r = await call(2, "kvdb.get", {"key": "llm.key"})
                check(r is not None and r["data"]["masked"] is True,
                      "reading it back is masked")
                r = await call(3, "kvdb.get",
                               {"key": "llm.key", "raw": True})
                check(r is not None
                      and r["data"]["value"] == "sk-e2e-secret",
                      "and raw:true returns exactly what was written")

                r = await call(4, "wifi.connect",
                               {"ssid": "AIPC", "psk": "mock-passphrase"})
                check(r is not None and r.get("ok"),
                      "Wi-Fi can be configured from the page")

                r = await call(5, "shell.exec", {"cmdline": "free"})
                check(r is not None and r["data"]["accepted"] is True,
                      "the console can run a command")
                saw_exit = False
                for _ in range(80):
                    m = await asyncio.wait_for(ws.receive(), timeout=8)
                    if m.type is aiohttp.WSMsgType.TEXT:
                        o = json.loads(m.data)
                        if o.get("type") == "log" \
                                and o["event"].get("exit") is not None:
                            saw_exit = True
                            break
                check(saw_exit,
                      "and sees the command finish, which is how the console "
                      "pane knows to stop showing it as running")
    finally:
        if mock is not None:
            mock.stop()
        proc.terminate()
        try:
            proc.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()


async def amain() -> None:
    await test_inbound_accepted()
    await test_wrong_token_refused()
    await test_no_hello_refused()
    await test_pin_mismatch_stops_board()
    await test_browser_https()
    await test_full_chain()


def main() -> int:
    test_cert_material()
    asyncio.run(amain())
    print("\n%d checks, %d failure(s)" % (CHECKS, len(FAILURES)))
    for f in FAILURES:
        print("  - %s" % f)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
