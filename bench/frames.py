"""The serial line formats of docs/PROTOCOL.md, on the bench side.

This is a second implementation of the protocol, written separately from the C core, so that the two can be tested
against each other.
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import reduce

LINE_MAX = 80

SENSOR_OFF = 0x01
NO_SENSOR = 0x02
PULSE_SEARCH = 0x04
LOW_PERFUSION = 0x08
INTERFERENCE = 0x10
FLAGS_ALL = 0x1F


def checksum(body: str) -> int:
    """XOR of every byte of the frame body."""
    return reduce(lambda acc, ch: acc ^ ord(ch), body, 0) & 0xFF


def _wrap(body: str) -> bytes:
    line = f"${body}*{checksum(body):02X}\r\n"
    if len(line) > LINE_MAX:
        raise ValueError(f"frame longer than {LINE_MAX} bytes: {line!r}")
    return line.encode("ascii")


@dataclass(frozen=True)
class OxFrame:
    """One oximeter frame. None means the value is absent ("--")."""

    clock: int
    spo2: int | None
    pr: int | None
    pi_centi: int | None
    flags: int = 0

    def encode(self) -> bytes:
        if not 0 <= self.clock <= 0xFFFFFFFF:
            raise ValueError("clock out of range")
        if self.spo2 is not None and not 0 <= self.spo2 <= 100:
            raise ValueError("spo2 out of range")
        if self.pr is not None and not 0 <= self.pr <= 400:
            raise ValueError("pr out of range")
        if self.pi_centi is not None and not 0 <= self.pi_centi <= 2000:
            raise ValueError("pi out of range")
        if self.flags & ~FLAGS_ALL:
            raise ValueError("unknown flag")
        spo2 = "--" if self.spo2 is None else str(self.spo2)
        pr = "--" if self.pr is None else str(self.pr)
        pi = "--" if self.pi_centi is None else f"{self.pi_centi // 100}.{self.pi_centi % 100:02d}"
        return _wrap(f"OX,{self.clock},{spo2},{pr},{pi},{self.flags:02X}")


@dataclass(frozen=True)
class BlFrame:
    """A command from the blender driver to the blender. FiO2 values per mille."""

    seq: int
    fio2_pm: int
    backup_pm: int
    alarm: int

    def encode(self) -> bytes:
        if not (0 <= self.seq <= 65535 and 210 <= self.fio2_pm <= 1000 and 210 <= self.backup_pm <= 1000):
            raise ValueError("field out of range")
        if not 0 <= self.alarm <= 3:
            raise ValueError("alarm out of range")
        return _wrap(f"BL,{self.seq},{self.fio2_pm},{self.backup_pm},{self.alarm}")


@dataclass(frozen=True)
class BsFrame:
    """The blender's status reply."""

    seq: int
    fio2_pm: int
    state: str

    def encode(self) -> bytes:
        if not (0 <= self.seq <= 65535 and 0 <= self.fio2_pm <= 1000) or self.state not in ("N", "H", "F"):
            raise ValueError("field out of range")
        return _wrap(f"BS,{self.seq},{self.fio2_pm},{self.state}")


class FrameError(ValueError):
    """A line that is not a valid frame."""


def _split(line: str) -> list[str]:
    if len(line) + 2 > LINE_MAX:
        raise FrameError("too_long")
    if not line.startswith("$"):
        raise FrameError("no_start")
    if len(line) < 4 or line[-3] != "*":
        raise FrameError("no_checksum")
    body, cs = line[1:-3], line[-2:]
    if any(c not in "0123456789ABCDEF" for c in cs) or "*" in body or "$" in body:
        raise FrameError("no_checksum")
    if not body:
        raise FrameError("empty")
    if checksum(body) != int(cs, 16):
        raise FrameError("checksum")
    return body.split(",")


def _uint(s: str, high: int) -> int:
    if not s or len(s) > 10 or not s.isascii() or not s.isdigit():
        raise FrameError("value")
    v = int(s)
    if v > high:
        raise FrameError("value")
    return v


def _opt_uint(s: str, high: int) -> int | None:
    return None if s == "--" else _uint(s, high)


def parse_ox(line: str) -> OxFrame:
    f = _split(line)
    if f[0] != "OX":
        raise FrameError("type")
    if len(f) != 6:
        raise FrameError("fields")
    pi: int | None
    if f[4] == "--":
        pi = None
    else:
        whole, dot, frac = f[4].rpartition(".")
        if dot != "." or len(frac) != 2:
            raise FrameError("value")
        pi = _uint(whole, 1000) * 100 + _uint(frac, 99)
        if pi > 2000:
            raise FrameError("value")
    if len(f[5]) != 2 or any(c not in "0123456789ABCDEF" for c in f[5]):
        raise FrameError("value")
    flags = int(f[5], 16)
    if flags & ~FLAGS_ALL:
        raise FrameError("value")
    return OxFrame(_uint(f[1], 0xFFFFFFFF), _opt_uint(f[2], 100), _opt_uint(f[3], 400), pi, flags)


def parse_bl(line: str) -> BlFrame:
    f = _split(line)
    if f[0] != "BL":
        raise FrameError("type")
    if len(f) != 5:
        raise FrameError("fields")
    fio2, backup = _uint(f[2], 1000), _uint(f[3], 1000)
    if fio2 < 210 or backup < 210:
        raise FrameError("value")
    return BlFrame(_uint(f[1], 65535), fio2, backup, _uint(f[4], 3))


def parse_bs(line: str) -> BsFrame:
    f = _split(line)
    if f[0] != "BS":
        raise FrameError("type")
    if len(f) != 4:
        raise FrameError("fields")
    if f[3] not in ("N", "H", "F"):
        raise FrameError("value")
    return BsFrame(_uint(f[1], 65535), _uint(f[2], 1000), f[3])


class LineSplitter:
    """Splits a byte stream into lines with the same rules as the C core's tc_line_feed."""

    def __init__(self) -> None:
        self._buf = bytearray()
        self._in_line = False
        self._overflow = False

    def feed(self, data: bytes) -> list[str]:
        out: list[str] = []
        for b in data:
            if b == 0x24:  # '$'
                self._in_line, self._overflow = True, False
                self._buf = bytearray(b"$")
            elif not self._in_line:
                continue
            elif b == 0x0A:  # '\n'
                line = bytes(self._buf)
                self._in_line = False
                if line.endswith(b"\r"):
                    line = line[:-1]
                if not self._overflow and len(line) + 2 <= LINE_MAX:
                    out.append(line.decode("ascii", errors="replace"))
            elif not self._overflow:
                if len(self._buf) >= LINE_MAX:
                    self._overflow = True
                else:
                    self._buf.append(b)
        return out
