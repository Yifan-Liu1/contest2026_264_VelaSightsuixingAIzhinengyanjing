#!/usr/bin/env python3
"""Write frames and log lines to the development machine's disk.

Layout, one directory per session:

    host/captures/<YYYYMMDD-HHMMSS>/
    ├── frame_00.jpg …      b64frames.py's naming, so
    │                       `ffmpeg -i frame_%02d.jpg` works unchanged
    ├── session.log         the log stream, including the dropped markers
    └── rejected/           frames that failed verification

Rejected frames are kept rather than discarded.  The hardware JPEG bitstream
defect fixed on 2026-08-17 was found precisely by keeping bad frames and
comparing them byte by byte on the host; every one of those frames had a
complete and legal marker structure and only the pixel content gave them away.
"It decodes" was never evidence, so it is not used as a reason to throw a frame
away here either.
"""

from __future__ import annotations

import os
import time
from typing import Optional

from board_link import fnv1a

DEFAULT_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "captures")


class CaptureSession:
    def __init__(self, root: str = DEFAULT_ROOT,
                 name: Optional[str] = None) -> None:
        self.root = root
        self.name = name or time.strftime("%Y%m%d-%H%M%S")
        self.dir = os.path.join(root, self.name)
        self.rejected_dir = os.path.join(self.dir, "rejected")
        self.frames = 0
        self.rejected = 0
        self.bytes = 0
        self.dropped = 0
        self.log_lines = 0
        self._log = None
        self._recording = False

    # ---- lifecycle -----------------------------------------------------

    def start(self) -> dict:
        os.makedirs(self.rejected_dir, exist_ok=True)
        self._log = open(os.path.join(self.dir, "session.log"), "a",
                         encoding="utf-8")
        self._recording = True
        return self.status()

    def stop(self) -> dict:
        self._recording = False
        if self._log is not None:
            self._log.close()
            self._log = None
        return self.status()

    @property
    def recording(self) -> bool:
        return self._recording

    def status(self) -> dict:
        return {
            "dir": self.dir,
            "name": self.name,
            "recording": self._recording,
            "frames": self.frames,
            "rejected": self.rejected,
            "bytes": self.bytes,
            "dropped": self.dropped,
            "log_lines": self.log_lines,
        }

    # ---- writing -------------------------------------------------------

    @staticmethod
    def verify(jpeg: bytes, digest: int) -> tuple[bool, str]:
        """Two independent checks, reported separately.

        The checksum says whether the transfer was faithful; the markers say
        whether the encoder produced a whole picture.  Collapsing them into one
        boolean is how "the picture is broken" and "the transfer is broken" get
        confused, which is the exact confusion this project has already paid
        for once.
        """
        if fnv1a(jpeg) != digest:
            return False, "fnv1a mismatch"
        if jpeg[:2] != b"\xff\xd8":
            return False, "no SOI"
        if jpeg[-2:] != b"\xff\xd9":
            return False, "no EOI"
        return True, "ok"

    def write_frame(self, seq: int, digest: int, jpeg: bytes) -> dict:
        if not self._recording:
            return {"written": False}

        ok, why = self.verify(jpeg, digest)
        if ok:
            path = os.path.join(self.dir, "frame_%02d.jpg" % self.frames)
            self.frames += 1
        else:
            path = os.path.join(self.rejected_dir,
                                "frame_%05d_%s.jpg"
                                % (seq, why.replace(" ", "_")))
            self.rejected += 1

        with open(path, "wb") as fp:
            fp.write(jpeg)
        self.bytes += len(jpeg)

        if not ok:
            self.write_log_text("capture: frame seq=%d rejected (%s), kept in "
                                "rejected/" % (seq, why))

        return {"written": True, "ok": ok, "why": why, "path": path}

    def write_log(self, event: dict) -> None:
        if not self._recording:
            return
        if "dropped" in event:
            self.dropped += int(event["dropped"])
            self.write_log_text("--- %d log line(s) dropped ---"
                                % int(event["dropped"]))
            return
        line = event.get("line")
        if line is None:
            return
        self.log_lines += 1
        self.write_log_text(line, t=event.get("t"))

    def write_log_text(self, text: str, t: Optional[int] = None) -> None:
        if self._log is None:
            return
        stamp = "%8d " % t if isinstance(t, int) else "         "
        self._log.write(stamp + text + "\n")
        self._log.flush()


def list_sessions(root: str = DEFAULT_ROOT) -> list:
    """Newest first, with enough detail for the export card on the page."""
    if not os.path.isdir(root):
        return []
    out = []
    for name in sorted(os.listdir(root), reverse=True):
        path = os.path.join(root, name)
        if not os.path.isdir(path):
            continue
        frames = len([f for f in os.listdir(path) if f.endswith(".jpg")])
        rej_dir = os.path.join(path, "rejected")
        rejected = len(os.listdir(rej_dir)) if os.path.isdir(rej_dir) else 0
        out.append({"name": name, "dir": path, "frames": frames,
                    "rejected": rejected})
    return out
