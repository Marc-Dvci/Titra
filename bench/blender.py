"""The simulated air-oxygen blender of docs/MODEL.md, with its own watchdog.

The watchdog stands for an independent safety circuit in the device. It is a separate implementation from the
blender driver's watchdog in the C core, so that one design mistake cannot sit in both layers.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field

from bench.frames import BlFrame, BsFrame
from bench.patient import lag

SERVO_RATE_PER_S = 0.05
MIX_TAU_S = 1.0
CIRCUIT_TAU_S = 6.0
HOLD_AFTER_S = 3.0
FALLBACK_AFTER_S = 15.0
BASELINE_WINDOW_S = 300.0


@dataclass
class BlenderEvent:
    t: float
    kind: str  # "frame", "hold", "fallback", "resume"
    fio2: float
    alarm: int


@dataclass
class Blender:
    backup_fio2: float
    fio2: float  # servo position
    t: float = 0.0
    state: str = "N"
    last_frame_t: float = 0.0
    last_seq: int = 0
    alarm: int = 0
    commanded: float = 0.0
    mixed: float = 0.0
    circuit: float = 0.0
    fallback_fio2: float = 0.0
    _history: deque[tuple[float, float]] = field(default_factory=deque)
    events: list[BlenderEvent] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.commanded = self.mixed = self.circuit = self.fallback_fio2 = self.fio2

    def on_frame(self, t: float, f: BlFrame) -> None:
        """Applies a command frame received at time t."""
        if self.state != "N":
            self.events.append(BlenderEvent(t, "resume", f.fio2_pm / 1000.0, f.alarm))
        self.state = "N"
        self.last_frame_t = t
        self.last_seq = f.seq
        self.alarm = f.alarm
        self.backup_fio2 = f.backup_pm / 1000.0
        self.commanded = f.fio2_pm / 1000.0
        self._history.append((t, self.commanded))
        while self._history and self._history[0][0] < t - BASELINE_WINDOW_S:
            self._history.popleft()
        self.events.append(BlenderEvent(t, "frame", self.commanded, f.alarm))

    def _fallback_value(self) -> float:
        if not self._history:
            return self.backup_fio2
        values = sorted(v for _, v in self._history)
        median_low = values[(len(values) - 1) // 2]
        return max(self.backup_fio2, median_low)

    def step(self, t: float, dt: float = 1.0) -> float:
        """Advances to time t and returns the FiO2 measured in the circuit."""
        quiet = t - self.last_frame_t
        if quiet >= FALLBACK_AFTER_S and self.state != "F":
            self.fallback_fio2 = self._fallback_value()
            self.state = "F"
            self.events.append(BlenderEvent(t, "fallback", self.fallback_fio2, 3))
        elif HOLD_AFTER_S <= quiet < FALLBACK_AFTER_S and self.state == "N":
            self.state = "H"
            self.events.append(BlenderEvent(t, "hold", self.commanded, 3))
        target = self.fallback_fio2 if self.state == "F" else self.commanded
        step = max(min(target - self.fio2, SERVO_RATE_PER_S * dt), -SERVO_RATE_PER_S * dt)
        self.fio2 += step
        self.mixed = lag(self.mixed, self.fio2, MIX_TAU_S, dt)
        self.circuit = lag(self.circuit, self.mixed, CIRCUIT_TAU_S, dt)
        self.t = t
        return self.circuit

    def status(self) -> BsFrame:
        return BsFrame(self.last_seq, int(round(self.circuit * 1000)), self.state)

    @property
    def alarm_sounding(self) -> bool:
        """The cot-side alarm: a high or medium alarm in the last frame, or the blender's own lost-link alarm."""
        return self.state != "N" or self.alarm >= 2
