#!/usr/bin/env python3
"""Receive JPEG frames that agent_camera pushed over TCP.

Usage: tcpframes.py OUTDIR [PORT] [COUNT]

The board opens one connection per frame and closes it when the frame is
complete (agent_camera_send_tcp()), so the connection boundary is the frame
boundary -- there is no framing to parse and nothing to unescape.

Each frame is checked the same way b64frames.py checks the console route:
SOI/EOI present, and the FNV-1a printed on the console can be compared with
the value reported here.  The console route needs those checks because the
mailbox bridge can silently truncate; TCP cannot, but the checks are cheap
and they are what makes "the picture is broken" separable from "the transfer
was broken".
"""
import os
import socket
import sys
import time

outdir = sys.argv[1]
port = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
want = int(sys.argv[3]) if len(sys.argv) > 3 else 1

os.makedirs(outdir, exist_ok=True)


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('0.0.0.0', port))
srv.listen(4)
print('listening on 0.0.0.0:%d, expecting %d frame(s)' % (port, want))

ok = 0
for i in range(want):
    conn, peer = srv.accept()
    t0 = time.time()
    chunks = []
    while True:
        b = conn.recv(65536)
        if not b:
            break
        chunks.append(b)
    conn.close()
    raw = b''.join(chunks)
    dt = time.time() - t0

    path = os.path.join(outdir, 'frame_%02d.jpg' % i)
    with open(path, 'wb') as f:
        f.write(raw)

    good = raw[:2] == b'\xff\xd8' and raw[-2:] == b'\xff\xd9'
    print('frame %02d from %s: %d bytes in %.3fs (%.1f KB/s) '
          'fnv1a=0x%08x SOI/EOI=%s -> %s'
          % (i, peer[0], len(raw), dt,
             len(raw) / dt / 1024 if dt > 0 else 0,
             fnv1a(raw), 'yes' if good else 'NO', path))
    if good:
        ok += 1

srv.close()
print('wrote %d/%d frame(s) to %s' % (ok, want, outdir))
