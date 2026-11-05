"""ctypes binding to the C core's simulation API (core/include/titra/sim.h).

The bench drives the same C code the QNX processes run. The library is built by `make -C core`; set TITRA_CORE_LIB
to use another build.
"""

from __future__ import annotations

import ctypes
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_LIB = ROOT / "core" / "build" / "libtitra_core.so"

MODE_CONTROL, MODE_HOLD, MODE_FALLBACK = 0, 1, 2
MODE_NAMES = {MODE_CONTROL: "control", MODE_HOLD: "hold", MODE_FALLBACK: "fallback"}

# Alarm condition bits (control.h).
A_SIGNAL_HOLD = 0x01
A_FALLBACK = 0x02
A_UNTRUSTED = 0x04
A_SPO2_LOW = 0x08
A_SPO2_VLOW = 0x10
A_SPO2_HIGH = 0x20
A_AT_MAX = 0x40


class TickOut(ctypes.Structure):
    _fields_ = [
        ("fio2", ctypes.c_double),
        ("ctl_fio2", ctypes.c_double),
        ("mode", ctypes.c_int),
        ("alarm", ctypes.c_int),
        ("alarms", ctypes.c_uint32),
        ("reasons", ctypes.c_uint32),
        ("adjusted", ctypes.c_int),
        ("refused", ctypes.c_int),
        ("sample_status", ctypes.c_int),
        ("spo2", ctypes.c_int),
    ]


def build() -> Path:
    """Builds the core library with make if it is missing."""
    if not DEFAULT_LIB.exists():
        subprocess.run(["make", "-s", "-C", str(ROOT / "core"), "build/libtitra_core.so"], check=True)
    return DEFAULT_LIB


_lib: ctypes.CDLL | None = None


def lib() -> ctypes.CDLL:
    global _lib
    if _lib is None:
        path = os.environ.get("TITRA_CORE_LIB") or str(build())
        cdll = ctypes.CDLL(path)
        cdll.tc_sim_size.restype = ctypes.c_size_t
        cdll.tc_sim_tick_size.restype = ctypes.c_size_t
        cdll.tc_sim_init.argtypes = [ctypes.c_void_p] + [ctypes.c_double] * 5
        cdll.tc_sim_init.restype = None
        cdll.tc_sim_param.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_double]
        cdll.tc_sim_param.restype = ctypes.c_int
        cdll.tc_sim_feed.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_double]
        cdll.tc_sim_feed.restype = ctypes.c_int
        cdll.tc_sim_tick.argtypes = [
            ctypes.c_void_p,
            ctypes.c_double,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.POINTER(TickOut),
        ]
        cdll.tc_sim_tick.restype = ctypes.c_size_t
        for name in ("tc_sim_frames_ok", "tc_sim_frames_rejected", "tc_sim_bytes_dropped"):
            fn = getattr(cdll, name)
            fn.argtypes = [ctypes.c_void_p]
            fn.restype = ctypes.c_uint32
        if cdll.tc_sim_tick_size() != ctypes.sizeof(TickOut):
            raise RuntimeError("TickOut does not match tc_sim_tick_t; rebuild the core")
        _lib = cdll
    return _lib


class OxFrameC(ctypes.Structure):
    _fields_ = [
        ("clock", ctypes.c_uint32),
        ("spo2", ctypes.c_int),
        ("pr", ctypes.c_int),
        ("pi_centi", ctypes.c_int),
        ("flags", ctypes.c_uint),
    ]


ABSENT = -1


def c_ox_parse(line: str) -> tuple[int, int | None, int | None, int | None, int] | None:
    """Parses a line with the C core's tc_ox_parse; None if the C parser rejects it."""
    cdll = lib()
    fn = cdll.tc_ox_parse
    fn.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(OxFrameC)]
    fn.restype = ctypes.c_int
    raw = line.encode("latin-1")
    f = OxFrameC()
    if fn(raw, len(raw), ctypes.byref(f)) != 0:
        return None

    def opt(v: int) -> int | None:
        return None if v == ABSENT else v

    return (int(f.clock), opt(f.spo2), opt(f.pr), opt(f.pi_centi), int(f.flags))


def c_ox_format(clock: int, spo2: int | None, pr: int | None, pi_centi: int | None, flags: int) -> bytes:
    """Formats a frame with the C core's tc_ox_format; empty if it refuses the values."""
    cdll = lib()
    fn = cdll.tc_ox_format
    fn.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(OxFrameC)]
    fn.restype = ctypes.c_size_t
    buf = ctypes.create_string_buffer(96)
    f = OxFrameC(
        clock,
        ABSENT if spo2 is None else spo2,
        ABSENT if pr is None else pr,
        ABSENT if pi_centi is None else pi_centi,
        flags,
    )
    n = fn(buf, len(buf), ctypes.byref(f))
    return buf.raw[:n]


class CotChain:
    """One cot's on-board chain (oximeter driver, controller, blender driver) running in the C core."""

    def __init__(
        self,
        fio2_min: float,
        fio2_max: float,
        backup_fio2: float,
        initial_fio2: float,
        now_s: float = 0.0,
        params: dict[str, float] | None = None,
    ) -> None:
        self._lib = lib()
        self._mem = ctypes.create_string_buffer(self._lib.tc_sim_size())
        self._lib.tc_sim_init(self._mem, fio2_min, fio2_max, backup_fio2, initial_fio2, now_s)
        for name, value in (params or {}).items():
            if self._lib.tc_sim_param(self._mem, name.encode(), float(value)) != 0:
                raise KeyError(f"unknown controller parameter: {name}")
        self._out = TickOut()
        self._bl = ctypes.create_string_buffer(96)

    def feed(self, data: bytes, rx_s: float) -> int:
        return int(self._lib.tc_sim_feed(self._mem, data, len(data), rx_s))

    def tick(self, now_s: float) -> tuple[bytes, TickOut]:
        n = self._lib.tc_sim_tick(self._mem, now_s, self._bl, len(self._bl), ctypes.byref(self._out))
        return self._bl.raw[:n], self._out

    @property
    def frames_ok(self) -> int:
        return int(self._lib.tc_sim_frames_ok(self._mem))

    @property
    def frames_rejected(self) -> int:
        return int(self._lib.tc_sim_frames_rejected(self._mem))

    @property
    def bytes_dropped(self) -> int:
        return int(self._lib.tc_sim_bytes_dropped(self._mem))
