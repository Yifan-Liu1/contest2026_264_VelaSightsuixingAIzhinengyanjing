#!/usr/bin/env python3
"""Time the board's TLS handshake by stamping its own log lines.

Why not the mbedTLS benchmark: its asymmetric section starts with
mbedtls_rsa_gen_key(), and RSA-2048 key generation on this part does not finish
in any reasonable time, so the run never reaches the numbers that matter.  The
handshake itself is the quantity of interest anyway -- it includes the network
round trips, which no component benchmark does.

Method: send `net_test` inside the agent, stamp the arrival of
"[vela_tls] Handshake start" and "[vela_tls] Handshake OK", and repeat.  The
first request pays a full handshake; later ones to the same host reuse the
pooled connection ("Reusing pooled connection"), so the difference between the
two is what a handshake costs on this hardware.

Usage: tls_handshake_time.py [repeats] [port]
"""

import os
import re
import sys
import termios
import time

PORT = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyUSB0"
REPEATS = int(sys.argv[1]) if len(sys.argv) > 1 else 4

MARKS = [
    ("start", re.compile(r"\[vela_tls\] Handshake start: Host=(\S+)")),
    ("ok", re.compile(r"\[vela_tls\] Handshake OK: (\S+) / (\S+)")),
    ("reuse", re.compile(r"Reusing pooled connection")),
    ("status", re.compile(r"HTTP Status: (\d+)")),
    ("fail", re.compile(r"(FAILED|TLS Error)")),
]

ANSI = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")


def configure(fd):
    a = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = a
    iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK
               | termios.ISTRIP | termios.INLCR | termios.IGNCR
               | termios.ICRNL | termios.IXON)
    oflag &= ~termios.OPOST
    lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON
               | termios.ISIG | termios.IEXTEN)
    cflag &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)
    cflag |= termios.CS8 | termios.CLOCAL | termios.CREAD
    if hasattr(termios, "CRTSCTS"):
        cflag &= ~termios.CRTSCTS
    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0
    ispeed = ospeed = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW,
                      [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])


def main():
    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    configure(fd)

    def send(text):
        os.write(fd, text.encode())

    def pump(seconds, on_line):
        deadline = time.time() + seconds
        buf = bytearray()
        while time.time() < deadline:
            try:
                data = os.read(fd, 4096)
            except (BlockingIOError, OSError):
                data = b""
            if not data:
                time.sleep(0.005)
                continue
            buf += data
            while b"\n" in buf:
                idx = buf.index(b"\n")
                raw = bytes(buf[:idx])
                del buf[:idx + 1]
                line = ANSI.sub(b"", raw).decode("utf-8", "replace")
                line = line.replace("\r", "").strip()
                if line and on_line(line, time.time()):
                    return True
        return False

    print("tls_handshake_time: %d run(s) on %s" % (REPEATS, PORT), flush=True)

    # Get into the AP console and the agent.  The bridge does not survive a
    # reset, so open it unconditionally; it is cheap to repeat.
    send("\x1d.\r\n")
    time.sleep(0.3)
    send("ap_console open\r\n")
    pump(1.5, lambda ln, t: False)
    send("ai_agent\r\n")
    pump(6.0, lambda ln, t: "vela>" in ln)

    results = []
    for run in range(REPEATS):
        state = {"t0": None, "start": None, "ok": None, "suite": None,
                 "reuse": False, "status": None, "done": False}

        t_send = time.time()

        def on_line(line, t):
            for name, rx in MARKS:
                m = rx.search(line)
                if not m:
                    continue
                if name == "start":
                    state["start"] = t
                elif name == "ok":
                    state["ok"] = t
                    state["suite"] = "%s / %s" % (m.group(1), m.group(2))
                elif name == "reuse":
                    state["reuse"] = True
                elif name == "status":
                    state["status"] = m.group(1)
                    return True
                elif name == "fail":
                    state["status"] = "FAILED"
                    return True
            return False

        send("net_test\r\n")
        pump(60.0, on_line)

        total = time.time() - t_send
        hs = (state["ok"] - state["start"]) if state["start"] and state["ok"] \
            else None
        results.append({
            "run": run + 1,
            "total_s": round(total, 3),
            "handshake_s": round(hs, 3) if hs is not None else None,
            "reused": state["reuse"],
            "suite": state["suite"],
            "status": state["status"],
        })
        print("  run %d: total %.3fs, handshake %s, reused=%s, %s, HTTP %s"
              % (run + 1, total,
                 ("%.3fs" % hs) if hs is not None else "-",
                 state["reuse"], state["suite"] or "-",
                 state["status"]), flush=True)
        time.sleep(1.0)

    os.close(fd)

    full = [r for r in results if r["handshake_s"] is not None]
    reused = [r for r in results if r["reused"]]
    print("\nsummary")
    if full:
        vals = sorted(r["handshake_s"] for r in full)
        print("  handshakes measured: %d, min %.3fs median %.3fs max %.3fs"
              % (len(vals), vals[0], vals[len(vals) // 2], vals[-1]))
        print("  ciphersuite: %s" % full[0]["suite"])
    tot = sorted(r["total_s"] for r in results)
    print("  request round trip: min %.3fs median %.3fs max %.3fs"
          % (tot[0], tot[len(tot) // 2], tot[-1]))
    print("  runs that reused a pooled connection: %d/%d"
          % (len(reused), len(results)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
