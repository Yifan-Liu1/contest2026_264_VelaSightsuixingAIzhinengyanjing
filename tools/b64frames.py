#!/usr/bin/env python3
"""Rebuild the JPEG frames that agent_camera printed on the console.

Usage: b64frames.py TRANSCRIPT OUTDIR

Each frame arrives as

    agent_camera: payload len=<n> fnv1a=0x<hash>
    -----BEGIN AGENT_CAMERA JPEG <n>-----
    <base64, wrapped>
    -----END AGENT_CAMERA JPEG-----

so the transcript carries its own length and hash: both are checked here
rather than trusting that the serial line delivered every byte.  Frames that
fail either check are reported and skipped instead of being written out as
plausible-looking garbage.
"""
import base64
import os
import re
import sys

src = sys.argv[1]
outdir = sys.argv[2]
os.makedirs(outdir, exist_ok=True)

text = open(src, errors="replace").read()


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


# The console interleaves its own lines ("ap0: ..."), so pull the payload
# hash/length pairs and the fenced blocks separately and pair them in order.
meta = re.findall(r"payload len=(\d+) fnv1a=0x([0-9a-f]{8})", text)
blocks = re.findall(
    r"-----BEGIN AGENT_CAMERA JPEG (\d+)-----\n(.*?)-----END AGENT_CAMERA JPEG-----",
    text, re.S)

print("found %d fenced block(s), %d payload header(s)" % (len(blocks), len(meta)))

ok = 0
for i, (declared, body) in enumerate(blocks):
    declared = int(declared)
    b64 = re.sub(r"[^A-Za-z0-9+/=]", "", body)
    try:
        raw = base64.b64decode(b64)
    except Exception as exc:                       # noqa: BLE001
        print("frame %02d: base64 refused (%s)" % (i, exc))
        continue

    if len(raw) != declared:
        print("frame %02d: length mismatch, got %d want %d"
              % (i, len(raw), declared))
        continue

    if i < len(meta) and fnv1a(raw) != int(meta[i][1], 16):
        print("frame %02d: fnv1a mismatch (serial corruption)" % i)
        continue

    if raw[:2] != b"\xff\xd8" or raw[-2:] != b"\xff\xd9":
        print("frame %02d: SOI/EOI missing" % i)
        continue

    path = os.path.join(outdir, "frame_%02d.jpg" % i)
    with open(path, "wb") as f:
        f.write(raw)
    ok += 1

print("wrote %d/%d frame(s) to %s" % (ok, len(blocks), outdir))
