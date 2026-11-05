"""Reference controllers for comparison (docs/MODEL.md): fixed FiO2 and a rule-based manual-care model."""

from __future__ import annotations

import random
from dataclasses import dataclass, field

LOW_ALARM_SPO2 = 88
LOW_ALARM_S = 10.0
HIGH_ALARM_SPO2 = 95
HIGH_ALARM_S = 60.0
RESPONSE_MEAN_S = 180.0
LOOK_AGAIN_S = 60.0
ROUND_S = 3600.0


@dataclass
class FixedFio2:
    fio2: float

    def step(self, t: float, spo2: int | None) -> float:
        return self.fio2


@dataclass
class ManualCare:
    """A nurse who answers alarms after a random delay, adjusts in fixed steps and checks every hour."""

    rng: random.Random
    fio2: float
    low_since: float | None = None
    high_since: float | None = None
    arrival: float | None = None
    look_again: float | None = None
    next_round: float = ROUND_S
    visits: int = field(default=0)

    def _adjust(self, spo2: int | None) -> None:
        if spo2 is None:
            return
        if spo2 < 80:
            self.fio2 += 0.10
        elif spo2 < LOW_ALARM_SPO2:
            self.fio2 += 0.05
        elif spo2 > HIGH_ALARM_SPO2 and self.fio2 > 0.21 + 1e-9:
            self.fio2 -= 0.05
        self.fio2 = round(min(max(self.fio2, 0.21), 1.0), 2)

    def step(self, t: float, spo2: int | None) -> float:
        low = spo2 is not None and spo2 < LOW_ALARM_SPO2
        high = spo2 is not None and spo2 > HIGH_ALARM_SPO2 and self.fio2 > 0.21 + 1e-9
        self.low_since = (self.low_since if self.low_since is not None else t) if low else None
        self.high_since = (self.high_since if self.high_since is not None else t) if high else None
        alarm = (self.low_since is not None and t - self.low_since >= LOW_ALARM_S) or (
            self.high_since is not None and t - self.high_since >= HIGH_ALARM_S
        )
        if alarm and self.arrival is None and self.look_again is None:
            self.arrival = t + self.rng.expovariate(1.0 / RESPONSE_MEAN_S)
        if self.arrival is not None and t >= self.arrival:
            self.arrival = None
            self.visits += 1
            self._adjust(spo2)
            self.look_again = t + LOOK_AGAIN_S
        elif self.look_again is not None and t >= self.look_again:
            self.look_again = None
            self._adjust(spo2)
        if t >= self.next_round:
            self.next_round += ROUND_S
            self._adjust(spo2)
        return self.fio2
