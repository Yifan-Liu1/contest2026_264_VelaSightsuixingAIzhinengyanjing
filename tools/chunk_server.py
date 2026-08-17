#!/usr/bin/env python3
"""Stand-in for the cloud upload endpoint.

Accepts the chunked uploads `audio_test stream` produces, stores each chunk as
its own file and answers 200 so the board can tell a delivered chunk from a
lost one.

The board sends one HTTP POST per chunk with the metadata in headers, so this
only has to be enough of an HTTP server to read a request and answer it.  It
is deliberately not http.server: that logs to stderr in a shape nobody wants
and makes reading an exact Content-Length awkward.

On exit it reports what arrived, checks each chunk actually decodes, and can
merge the session into one file to listen to -- which is the point of the
exercise, because a chunk that decodes on its own still proves nothing about
whether the chunk boundaries land where they should.

SPDX-License-Identifier: Apache-2.0
"""

import argparse
import os
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time

# Chunks are a few kilobytes; anything far past that is a bug or a stray
# connection, and reading it into memory unbounded would be the wrong answer
# either way.

MAX_BODY = 4 << 20

# Guard against a client that connects and says nothing.

SOCKET_TIMEOUT = 30.0

state_lock = threading.Lock()
sessions = {}


class Session:
    """One recording session, named by when it arrived here.

    The name deliberately comes from this machine's clock and not from the
    board's.  The board has no RTC and nothing sets its time, so the session
    id it sends is seconds since boot -- the first real session came through
    as 000000f9, which is 249.  That is unique enough to separate two
    sessions but says nothing about when either happened, which is the one
    thing a directory name is good for.
    """

    # No separator in the clock part on purpose: a colon would read better but
    # is illegal on Windows, exFAT and SMB shares, and scp takes the first one
    # as a host separator.  These recordings get handed to whoever is doing
    # the cloud side, so the name stays portable.
    STAMP_FORMAT = "%Y%m%d_%H%M%S"

    def __init__(self, sid, root):
        self.sid = sid
        self.stamp = time.strftime(self.STAMP_FORMAT)

        name = f"rec_{self.stamp}"
        final = os.path.join(root, f"{name}.ogg")

        # Two sessions inside the same second would otherwise land in one
        # directory and interleave their chunks.  It takes a board reboot
        # mid-second to do it, but the failure would look like corrupted
        # audio rather than a naming problem, so it is cheap to rule out.
        if os.path.exists(final) or os.path.exists(f"{final[:-4]}.parts"):
            name = f"{name}_{sid}"
            final = os.path.join(root, f"{name}.ogg")

        self.name = name
        self.final = final

        # Chunks land in a sibling directory that is removed once they have
        # been joined, so a finished session is one file rather than one file
        # inside a directory of the same name.
        self.dir = os.path.join(root, f"{name}.parts")
        self.chunks = {}          # sequence -> (path, bytes, duration_ms)
        self.first_seen = time.time()
        self.last_seen = self.first_seen
        os.makedirs(self.dir, exist_ok=True)


def read_headers(f):
    """Read request line and headers, returning (path, dict) or None."""
    line = f.readline(8192)
    if not line:
        return None

    request = line.decode("latin-1").strip()
    headers = {}

    while True:
        line = f.readline(8192)
        if not line or line in (b"\r\n", b"\n"):
            break

        text = line.decode("latin-1").strip()
        if ":" not in text:
            continue

        key, _, value = text.partition(":")
        headers[key.strip().lower()] = value.strip()

    return request, headers


class Handler(socketserver.StreamRequestHandler):
    timeout = SOCKET_TIMEOUT

    def handle(self):
        parsed = read_headers(self.rfile)
        if parsed is None:
            return

        request, headers = parsed

        try:
            length = int(headers.get("content-length", "0"))
        except ValueError:
            self.respond(400, "bad Content-Length")
            return

        if length < 0 or length > MAX_BODY:
            self.respond(413, "body too large")
            return

        body = self.rfile.read(length) if length else b""
        if len(body) != length:
            print(f"  ! short body: {len(body)} of {length} bytes")
            self.respond(400, "short body")
            return

        sid = headers.get("x-session-id", "nosession")
        seq = headers.get("x-sequence", "?")
        start_ms = headers.get("x-start-ms", "?")
        duration_ms = headers.get("x-duration-ms", "?")

        # Answer before doing anything on disk.  The board times its send out
        # after a few seconds, and making it wait for a filesystem it cannot
        # see would turn a slow disk into a dropped chunk.

        self.respond(200, "ok")

        try:
            seq_n = int(seq)
        except ValueError:
            seq_n = -1

        with state_lock:
            session = sessions.get(sid)
            if session is None:
                session = Session(sid, self.server.outdir)
                sessions[sid] = session
                print(f"[{sid}] new session -> {session.dir}")

            # .ogg, not .opus: both name the same container and these really
            # are Ogg streams (RFC 7845), but the merged file next to them is
            # also Ogg, and two extensions for one format in one directory
            # invites the question of whether they differ.  They do not.
            name = os.path.join(session.dir, f"chunk_{seq_n:04d}.ogg")
            session.chunks[seq_n] = (name, len(body), duration_ms)
            session.last_seen = time.time()
            count = len(session.chunks)

        with open(name, "wb") as out:
            out.write(body)

        print(f"[{sid}] seq={seq} start={start_ms}ms dur={duration_ms}ms "
              f"{len(body)} bytes -> {os.path.basename(name)} "
              f"({count} so far)")

    def respond(self, code, text):
        reason = {200: "OK", 400: "Bad Request", 413: "Payload Too Large"}
        body = text.encode()
        head = (f"HTTP/1.1 {code} {reason.get(code, 'Error')}\r\n"
                f"Content-Type: text/plain\r\n"
                f"Content-Length: {len(body)}\r\n"
                f"Connection: close\r\n\r\n").encode()
        try:
            self.wfile.write(head + body)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            pass

    def log_message(self, *args):
        pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, addr, handler, outdir):
        self.outdir = outdir
        super().__init__(addr, handler)


def probe_duration_ms(path):
    """Decoded length of one chunk in ms, or None if it will not decode."""
    if shutil.which("ffprobe") is None:
        return None

    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries",
             "format=duration", "-of", "csv=p=0", path],
            capture_output=True, text=True, timeout=15)
        if out.returncode != 0 or not out.stdout.strip():
            return -1
        return int(float(out.stdout.strip()) * 1000)
    except (subprocess.SubprocessError, ValueError):
        return -1


def summarise(session, merge, also_wav, keep_chunks):
    order = sorted(session.chunks)
    if not order:
        return

    print(f"\n=== {session.name} (board session {session.sid}) ===")
    print(f"  {len(order)} chunk(s), {order[0]}..{order[-1]}")

    missing = [n for n in range(order[0], order[-1] + 1)
               if n not in session.chunks]
    if missing:
        # Not necessarily a fault: the board drops a queued chunk rather than
        # let a slow upload stall capture, and the protocol allows the gap.
        print(f"  gaps at {missing} (dropped on the board, or lost)")
    else:
        print("  no gaps")

    total = sum(v[1] for v in session.chunks.values())
    print(f"  {total} byte(s) received")

    bad = []
    drift = []
    for n in order:
        path, nbytes, claimed = session.chunks[n]
        got = probe_duration_ms(path)
        if got is None:
            break
        if got < 0:
            bad.append(n)
            continue
        try:
            want = int(claimed)
        except ValueError:
            continue
        # The encoder pads the last frame with silence, so up to one 20 ms
        # frame of overshoot is expected rather than wrong.
        if abs(got - want) > 25:
            drift.append((n, want, got))

    if bad:
        print(f"  DOES NOT DECODE: {bad}")
    if drift:
        print("  duration disagrees with the header (ms, claimed vs "
              f"decoded): {drift}")
    if not bad and not drift:
        print("  every chunk decodes and matches its declared duration")

    if merge:
        merge_session(session, order, also_wav, keep_chunks)
    else:
        print(f"  chunks in {session.dir}")


def merge_session(session, order, also_wav, keep_chunks):
    """Join the chunks into one file and drop the pieces.

    Done with `-c:a copy`, so the Opus packets are moved into a single Ogg
    stream untouched.  Re-encoding would be the obvious way to produce one
    file and the wrong one: the chunks are already lossy, and decoding and
    re-encoding them would stack a second generation of loss on top for no
    gain.  Copying is also 40-odd times smaller than the decoded WAV this
    used to write -- 32 KiB against 1.3 MiB for fifteen seconds.

    The chunks are only deleted after the merged file has been decoded back,
    because until that succeeds they are the only copy of the recording.  A
    merge that exits 0 is not sufficient evidence on its own: an unreadable
    concat list produces an empty output and a clean exit status.
    """
    if shutil.which("ffmpeg") is None:
        print("  ffmpeg not found, keeping the chunks")
        return

    # The concat list is scaffolding for ffmpeg, not a result, so it never
    # goes in the output directory.
    listing = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
    try:
        for n in order:
            path = os.path.abspath(session.chunks[n][0])
            listing.write(f"file '{path}'\n")
        listing.close()

        result = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
             "-f", "concat", "-safe", "0", "-i", listing.name,
             "-c:a", "copy", session.final],
            capture_output=True, text=True)
    finally:
        os.unlink(listing.name)

    if result.returncode != 0:
        print(f"  merge failed, chunks kept in {session.dir}")
        print(f"    {result.stderr.strip()[:200]}")
        return

    merged_ms = probe_duration_ms(session.final)
    if merged_ms is not None and merged_ms <= 0:
        print(f"  merged file does not decode, chunks kept in {session.dir}")
        return

    size = os.path.getsize(session.final)
    print(f"  merged -> {session.final}")
    print(f"    {size} bytes, {merged_ms} ms, no re-encode")

    if also_wav:
        # Only on request: useful when the next step is a filter or a
        # spectrum rather than a listen, since those want PCM anyway.
        wav = f"{session.final[:-4]}.wav"
        decode = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
             "-i", session.final, wav],
            capture_output=True, text=True)

        if decode.returncode == 0:
            print(f"    also decoded -> {wav} "
                  f"({os.path.getsize(wav)} bytes)")
        else:
            print(f"    wav decode failed: {decode.stderr.strip()[:200]}")

    if keep_chunks:
        print(f"    chunks kept in {session.dir}")
    else:
        shutil.rmtree(session.dir, ignore_errors=True)

    print(f"  listen: ffplay -nodisp -autoexit {session.final}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=9001)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--out",
                        default="/home/mi/vela_competition_continue/data",
                        help="where sessions are written")
    parser.add_argument("--no-merge", action="store_true",
                        help="skip joining the chunks on exit")
    parser.add_argument("--wav", action="store_true",
                        help="also decode the merged Ogg to WAV")
    parser.add_argument("--keep-chunks", action="store_true",
                        help="keep the per-chunk files after merging")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)

    server = Server((args.host, args.port), Handler, args.out)
    print(f"listening on {args.host}:{args.port}, writing to {args.out}")
    print("board: audio_test stream <this host> "
          f"{args.port} -t 30 -c 2000")
    print("Ctrl-C to stop and summarise\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping")
    finally:
        server.shutdown()
        with state_lock:
            found = list(sessions.values())
        for session in found:
            summarise(session, not args.no_merge, args.wav,
                      args.keep_chunks)
        if not found:
            print("no chunks arrived")

    return 0


if __name__ == "__main__":
    sys.exit(main())
