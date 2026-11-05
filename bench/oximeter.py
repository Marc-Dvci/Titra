"""The simulated oximeter of docs/MODEL.md, with the bench's fault injection (docs/SPEC.md, fault matrix)."""

from __future__ import annotations

import random
from collections import deque
from dataclasses import dataclass, field

from bench.frames import INTERFERENCE, SENSOR_OFF, OxFrame

AVERAGING_S = 8
NOISE_SD = 1.0


@dataclass
class OximeterParams:
    artefact_flagged_per_h: float = 2.0
    artefact_unflagged_per_h: float = 0.5
    sensor_off_per_h: float = 0.5
    sensor_off_duration_s: tuple[float, float] = (10.0, 120.0)


@dataclass
class _Episode:
    kind: str
    left: float
    offset: float = 0.0


@dataclass
class Oximeter:
    """One frame per second from the saturation (0 to 1) and pulse rate at the sensor site."""

    rng: random.Random
    params: OximeterParams = field(default_factory=OximeterParams)
    clock: int = 0
    fixed_spo2: int | None = None  # step tests: report this SpO2 whatever the patient
    _avg: deque[float] = field(default_factory=lambda: deque(maxlen=AVERAGING_S))
    _episode: _Episode | None = None
    _pi: float = 1.5
    _forced: str | None = None  # bench fault in progress
    _last: OxFrame | None = None
    _frozen: OxFrame | None = None
    _frames_since_fault: int = 0

    def __post_init__(self) -> None:
        self.clock = self.rng.randrange(1000, 100000)
        self._pi = self.rng.uniform(1.0, 2.5)

    # Fault injection, driven by the bench.
    def force(self, fault: str | None) -> None:
        """Starts or ends a fault: sensor_off, cable_cut, repeat_frame, frozen_values, garbage_line or None."""
        self._forced = fault
        self._frozen = self._last if fault == "frozen_values" else None
        self._frames_since_fault = 0

    def _natural_episode(self) -> None:
        if self._episode is not None:
            self._episode.left -= 1.0
            if self._episode.left <= 0.0:
                self._episode = None
            return
        p, r = self.params, self.rng
        u = r.random() * 3600.0
        if u < p.sensor_off_per_h:
            self._episode = _Episode("sensor_off", r.uniform(*p.sensor_off_duration_s))
        elif u < p.sensor_off_per_h + p.artefact_flagged_per_h:
            self._episode = _Episode("flagged", r.uniform(5.0, 30.0), r.uniform(5.0, 20.0))
        elif u < p.sensor_off_per_h + p.artefact_flagged_per_h + p.artefact_unflagged_per_h:
            self._episode = _Episode("unflagged", r.uniform(5.0, 20.0), r.uniform(5.0, 15.0))

    def measure(self, sa_site: float, pr_true: float) -> OxFrame:
        """The frame the oximeter would send now, before any bench fault."""
        self.clock = (self.clock + 1) & 0xFFFFFFFF
        self._avg.append(sa_site)
        self._natural_episode()
        self._pi = min(max(self._pi + self.rng.gauss(0.0, 0.02), 0.3), 5.0)
        pr = int(round(min(max(pr_true + self.rng.gauss(0.0, 2.0), 0.0), 400.0)))
        pi = int(round(self._pi * 100))
        if self.fixed_spo2 is not None:
            return OxFrame(self.clock, self.fixed_spo2, pr, pi, 0)
        reading = 100.0 * sum(self._avg) / len(self._avg) + self.rng.gauss(0.0, NOISE_SD)
        ep = self._episode
        if ep is not None and ep.kind == "sensor_off":
            return OxFrame(self.clock, None, None, None, SENSOR_OFF)
        flags = 0
        if ep is not None:
            reading -= ep.offset
            pi = max(pi // 2, 10)
            if ep.kind == "flagged":
                flags = INTERFERENCE
        spo2 = int(round(min(max(reading, 0.0), 100.0)))
        return OxFrame(self.clock, spo2, pr, pi, flags)

    def transmit(self, sa_site: float, pr_true: float) -> bytes:
        """The bytes on the serial line this second, with any bench fault applied."""
        frame = self.measure(sa_site, pr_true)
        fault = self._forced
        self._frames_since_fault += 1
        if fault == "cable_cut":
            return b""
        if fault == "sensor_off":
            frame = OxFrame(frame.clock, None, None, None, SENSOR_OFF)
        elif fault == "repeat_frame" and self._last is not None:
            return self._last.encode()
        elif fault == "frozen_values" and self._frozen is not None:
            fz = self._frozen
            frame = OxFrame(frame.clock, fz.spo2, fz.pr, fz.pi_centi, fz.flags)
        self._last = frame
        data = frame.encode()
        if fault == "garbage_line":
            noise = bytes(self.rng.choice(b"0123456789ABCDEF,.*\r#@!") for _ in range(40))
            if self._frames_since_fault % 3 == 0:
                # Corrupt one character of the frame body: the checksum no longer matches.
                i = self.rng.randrange(1, len(data) - 5)
                b = bytearray(data)
                b[i] = ord("7") if b[i] != ord("7") else ord("8")
                data = bytes(b)
            data = noise + data
        return data
