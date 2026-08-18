#!/usr/bin/env python3
"""Getting from "nothing" to "a TCP link", in four steps.

The board is auto-starting its own network now (bk7258_net_autostart.c), so on
a good day step 1 is the whole story.  The later steps exist because auto-start
can fail -- no credentials stored, the access point moved, DHCP did not answer
-- and without them a failed auto-start would lock the operator out of the
tool whose entire purpose is to debug the board.

  1. try the address that worked last time
  2. open the serial port, read `ifconfig wlan0`, try that address, close it
  3. open the serial port, run the whole provisioning sequence, close it
  4. give up on TCP and say which step failed

Every serial step closes the port before returning.  That is not tidiness: any
open handle makes serial_cmd.sh and autoflash.sh refuse to run, so a backend
that kept the port would break the flashing workflow this tool is supposed to
support.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from typing import Optional

from board_link import DEFAULT_PORT as TCP_PORT
from board_link import BoardLink
from serial_console import SerialBusy, SerialConsole

CACHE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          ".last_board.json")


@dataclass
class BootstrapResult:
    ok: bool
    ip: Optional[str] = None
    path: str = ""              # which step succeeded, or where it stopped
    steps: list = field(default_factory=list)
    degraded_reason: str = ""
    serial_available: bool = True


def load_cached_ip() -> Optional[str]:
    try:
        with open(CACHE_PATH, "r", encoding="utf-8") as fp:
            return json.load(fp).get("ip")
    except (OSError, ValueError):
        return None


def save_cached_ip(ip: str) -> None:
    try:
        with open(CACHE_PATH, "w", encoding="utf-8") as fp:
            json.dump({"ip": ip}, fp)
    except OSError:
        pass


class Bootstrap:
    def __init__(self, port: int = TCP_PORT, serial_port: str = "/dev/ttyUSB0",
                 ssid: str = "", psk: str = "",
                 serial_factory=None, tcp_port: Optional[int] = None) -> None:
        self.port = tcp_port or port
        self.serial_port = serial_port
        self.ssid = ssid
        self.psk = psk
        # Injectable so the end-to-end tests can drive a pty instead of real
        # hardware, and so a machine with no board attached still exercises
        # every branch here.
        self.serial_factory = serial_factory or (
            lambda: SerialConsole(self.serial_port))

    async def _try_tcp(self, ip: str, result: BootstrapResult,
                       label: str) -> bool:
        link = BoardLink(ip, self.port)
        ok = await link.probe()
        result.steps.append({"step": label, "ip": ip, "ok": ok,
                             "detail": link.last_error or "sys.status answered"})
        await link.close()
        if ok:
            result.ok = True
            result.ip = ip
            result.path = label
            save_cached_ip(ip)
        return ok

    async def run(self) -> BootstrapResult:
        result = BootstrapResult(ok=False)

        # ---- 1. the address that worked last time ----------------------
        cached = load_cached_ip()
        if cached:
            if await self._try_tcp(cached, result, "cached-ip"):
                return result
        else:
            result.steps.append({"step": "cached-ip", "ok": False,
                                 "detail": "no cached address yet"})

        # ---- 2. ask the board over serial what its address is ----------
        try:
            with self.serial_factory() as con:
                con.open_bridge()
                running, addr, _text = con.read_wlan()
        except SerialBusy as exc:
            result.steps.append({"step": "serial-ifconfig", "ok": False,
                                 "detail": str(exc)})
            result.serial_available = False
            result.degraded_reason = str(exc)
            return result
        except OSError as exc:
            result.steps.append({"step": "serial-ifconfig", "ok": False,
                                 "detail": "%s: %s" % (type(exc).__name__, exc)})
            result.serial_available = False
            result.degraded_reason = "serial port unavailable: %s" % exc
            return result

        result.steps.append({
            "step": "serial-ifconfig", "ok": bool(running), "ip": addr,
            "detail": "state/address read from ifconfig" if running else
                      "not RUNNING, or still on the default %s" % (addr or "-"),
        })

        if running and addr:
            if await self._try_tcp(addr, result, "serial-ifconfig+tcp"):
                return result

        # ---- 3. provision from scratch ---------------------------------
        if not self.ssid:
            result.degraded_reason = (
                "the board is not on the network and no SSID was given to "
                "provision it with")
            result.steps.append({"step": "serial-provision", "ok": False,
                                 "detail": result.degraded_reason})
            return result

        try:
            with self.serial_factory() as con:
                con.open_bridge()
                ok, addr, text = con.provision(self.ssid, self.psk)
        except (SerialBusy, OSError) as exc:
            result.steps.append({"step": "serial-provision", "ok": False,
                                 "detail": str(exc)})
            result.degraded_reason = str(exc)
            return result

        result.steps.append({"step": "serial-provision", "ok": bool(ok),
                             "ip": addr,
                             "detail": text[-400:] if text else ""})

        if ok and addr:
            if await self._try_tcp(addr, result, "serial-provision+tcp"):
                return result

        # ---- 4. degraded ------------------------------------------------
        result.degraded_reason = (
            "provisioning ran but no TCP link came up; the page is in "
            "serial-only mode")
        return result
