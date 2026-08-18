#!/usr/bin/env python3
"""Serial link to the board: NSH text, and only for what TCP cannot do.

This is not a second transport.  The board's web_tool service does not live on
the serial line at all -- what is on the serial line is the NSH console, which
is a different thing with a different protocol.  Treating the two as
interchangeable would mean inventing an abstraction that fits neither, so this
module is openly limited to the three jobs TCP cannot do:

  1. bootstrap -- get the board onto the network and read its address
  2. early log -- the boot messages that exist before there is a network
  3. rescue    -- kvdb set / reboot when TCP is not answering at all

Port contention is the hard constraint.  serial_cmd.sh refuses to run when
/dev/ttyUSB0 is busy, and autoflash.sh needs it exclusively.  A backend that
held the port open would break both, so this class opens the port only for the
duration of a call and closes it again, and the refusal message matches
serial_cmd.sh's so the two failures look the same to whoever reads them.

pyserial is present on this machine (3.5), but the standard library's termios
is used anyway: it is the same configuration serial_cmd.sh applies with stty,
it removes one thing that can be missing from a fresh checkout, and there is
nothing here that needs more than raw bytes at a fixed baud rate.
"""

from __future__ import annotations

import os
import re
import subprocess
import termios
import time
from typing import Optional

DEFAULT_PORT = "/dev/ttyUSB0"

# Fixed by the CP build (CONFIG_UART_PRINT_BAUD_RATE=115200).  Not to be
# confused with autoflash.sh's -b, which is only the bootrom download rate.
# Reading at the old 921600 returns 0x80/0x00 garbage, which looks exactly
# like a dead board.
BAUD = 115200

ANSI_RE = re.compile(rb"\x1b\[[0-9;?]*[a-zA-Z]")

# `ifconfig wlan0` line: state word and address.  The state has to be RUNNING
# and the address has to differ from NuttX's default static 10.0.0.2 -- see
# docs/WiFi使用说明.md.  ping is not usable as a criterion here (this network
# blocks ICMP) and neither is the agent's "Network connected: yes" (true even
# with the interface down).
IFCONFIG_STATE_RE = re.compile(r"at\s+(\w+)")
IFCONFIG_ADDR_RE = re.compile(r"inet addr:(\d+\.\d+\.\d+\.\d+)")

NUTTX_DEFAULT_ADDR = "10.0.0.2"


class SerialBusy(RuntimeError):
    """The port is held by something else -- screen, cat, or autoflash.sh."""


def port_holder(port: str = DEFAULT_PORT) -> Optional[str]:
    """Who has the port, in the words serial_cmd.sh uses.  None when free."""
    try:
        out = subprocess.run(["fuser", "-v", port], capture_output=True,
                             text=True, timeout=5)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    if out.returncode != 0:
        return None
    text = (out.stdout + out.stderr).strip()
    return text or "unknown process"


class SerialConsole:
    """Open the port, talk, close it.  Never held between calls."""

    def __init__(self, port: str = DEFAULT_PORT, baud: int = BAUD,
                 check_holder: bool = True) -> None:
        self.port = port
        self.baud = baud
        # The fuser guard is about contention for a real serial port, where
        # screen, cat and autoflash.sh all want it exclusively.  The
        # end-to-end tests drive a pty whose other end is necessarily held by
        # the mock, so they turn it off; nothing else should.
        self.check_holder = check_holder
        self.fd: Optional[int] = None
        self.transcript = ""

    # ---- open / close --------------------------------------------------

    def __enter__(self) -> "SerialConsole":
        self.open()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def open(self) -> None:
        holder = port_holder(self.port) if self.check_holder else None
        if holder is not None:
            raise SerialBusy(
                "串口被占用，先关掉 screen/cat：\n  " + holder.replace("\n", "\n  "))
        self.fd = os.open(self.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        self._configure()

    def close(self) -> None:
        if self.fd is not None:
            try:
                os.close(self.fd)
            finally:
                self.fd = None

    def _configure(self) -> None:
        """115200 cs8 -cstopb -parenb raw -echo -crtscts, as stty would."""
        assert self.fd is not None
        try:
            attrs = termios.tcgetattr(self.fd)
        except termios.error:
            # A pty created by the tests has no baud settings worth touching;
            # raw mode is what matters and the default is already close.
            return
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs

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

        speed = getattr(termios, "B%d" % self.baud, None)
        if speed is not None:
            ispeed = ospeed = speed

        termios.tcsetattr(self.fd, termios.TCSANOW,
                          [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])

    # ---- raw io --------------------------------------------------------

    def write(self, text: str) -> None:
        assert self.fd is not None
        os.write(self.fd, text.encode())

    def drain(self, seconds: float) -> str:
        """Read for a fixed period.  Returns decoded, de-escaped text."""
        assert self.fd is not None
        deadline = time.time() + seconds
        chunks = bytearray()
        while time.time() < deadline:
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                data = b""
            except OSError:
                break
            if data:
                chunks += data
            else:
                time.sleep(0.02)
        text = ANSI_RE.sub(b"", bytes(chunks)).decode("utf-8", "replace")
        text = text.replace("\r", "")
        self.transcript += text
        return text

    def command(self, cmd: str, wait: float = 1.5) -> str:
        """Send one line and read the reply.

        The reader has to be running before the write for a stream, but here
        the kernel buffers for us, so a write followed by a timed read is
        enough -- and much simpler than serial_cmd.sh's background cat.
        """
        self.write(cmd + "\r\n")
        return self.drain(wait)

    def open_bridge(self, wait: float = 1.2) -> str:
        """Switch the shared UART from the CP shell to the AP console.

        The bridge does not survive a reset, so this is idempotent and cheap
        to repeat rather than something to track.
        """
        self.write("\x1d")
        self.write(".")
        self.write("\r\n")
        time.sleep(0.2)
        return self.command("ap_console open", wait)

    # ---- the three jobs -------------------------------------------------

    def read_wlan(self, wait: float = 1.5) -> tuple[bool, Optional[str], str]:
        """(running_with_a_real_address, address, transcript)."""
        text = self.command("ifconfig wlan0", wait)
        addr = None
        state = None
        for line in text.splitlines():
            if "wlan0" in line:
                m = IFCONFIG_STATE_RE.search(line)
                if m:
                    state = m.group(1)
            m = IFCONFIG_ADDR_RE.search(line)
            if m and addr is None:
                addr = m.group(1)
        ok = state == "RUNNING" and addr is not None \
            and addr != NUTTX_DEFAULT_ADDR
        return ok, addr, text

    def provision(self, ssid: str, psk: str = "",
                  progress=None) -> tuple[bool, Optional[str], str]:
        """The full manual sequence, with the retry the first renew needs."""
        log = []

        def step(cmd: str, wait: float = 2.0) -> str:
            out = self.command(cmd, wait)
            log.append("$ %s\n%s" % (cmd, out))
            if progress is not None:
                progress(cmd, out)
            return out

        step("ifup wlan0")
        if psk:
            step("wapi psk wlan0 %s 3" % psk)
        step("wapi essid wlan0 %s 1" % ssid, wait=4.0)

        # Two attempts, because the first one after association is measured to
        # fail: there is a window between "associated" and "can exchange
        # DHCP".  Reporting the first failure as the outcome would call a
        # working network broken every time.
        for attempt in range(2):
            step("renew wlan0", wait=6.0)
            ok, addr, text = self.read_wlan()
            log.append(text)
            if ok:
                return True, addr, "\n".join(log)
        ok, addr, text = self.read_wlan()
        log.append(text)
        return ok, addr, "\n".join(log)

    def rescue(self, cmds: list[str], wait: float = 2.0) -> str:
        """Run arbitrary console commands.  The way back in when TCP is dead."""
        out = []
        for cmd in cmds:
            out.append("$ %s\n%s" % (cmd, self.command(cmd, wait)))
        return "\n".join(out)
