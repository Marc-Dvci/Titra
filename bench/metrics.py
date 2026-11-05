"""Outcome measures for closed-loop runs and step tests (docs/SPEC.md, "Evaluation")."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import asdict, dataclass

ROOM_AIR = 0.21


@dataclass(frozen=True)
class Second:
    """One second of a run."""

    t: float
    sa_true: float  # saturation at the sensor site, 0 to 1 (simulation only)
    spo2: int | None  # displayed SpO2, None when absent
    flags: int
    fio2: float  # FiO2 commanded at the blender
    circuit: float  # FiO2 measured in the circuit
    mode: int
    alarm: int


def in_target(spo2: float, fio2: float) -> bool:
    """The trial's definition: 91-95% while receiving oxygen, 91-100% in room air."""
    if fio2 <= ROOM_AIR + 1e-9:
        return spo2 >= 91.0
    return 91.0 <= spo2 <= 95.0


def max_change_in_window(values: Sequence[float], window: int, direction: int) -> float:
    """The largest change over any `window` consecutive seconds, in the given direction (+1 up, -1 down)."""
    best = 0.0
    for i in range(len(values) - window):
        best = max(best, direction * (values[i + window] - values[i]))
    return best


@dataclass(frozen=True)
class Summary:
    seconds: int
    valid_pct: float
    time_in_target_pct: float
    below_85_pct: float
    below_80_pct: float
    above_98_on_o2_pct: float
    true_time_in_target_pct: float
    true_below_80_pct: float
    mean_fio2: float
    adjustments_per_min: float
    max_rise_60s: float
    max_fall_60s: float
    hold_s: int
    fallback_s: int

    def as_dict(self) -> dict[str, float]:
        return {k: float(v) for k, v in asdict(self).items()}


def summarise(trace: Sequence[Second]) -> Summary:
    n = len(trace)
    valid = [s for s in trace if s.spo2 is not None and s.flags == 0]
    nv = max(len(valid), 1)

    def pct(count: int, total: int) -> float:
        return 100.0 * count / total if total else 0.0

    fio2 = [s.fio2 for s in trace]
    adjustments = sum(1 for a, b in zip(fio2, fio2[1:], strict=False) if abs(b - a) > 1e-9)
    return Summary(
        seconds=n,
        valid_pct=pct(len(valid), n),
        time_in_target_pct=pct(sum(1 for s in valid if s.spo2 is not None and in_target(s.spo2, s.fio2)), nv),
        below_85_pct=pct(sum(1 for s in valid if s.spo2 is not None and s.spo2 < 85), nv),
        below_80_pct=pct(sum(1 for s in valid if s.spo2 is not None and s.spo2 < 80), nv),
        above_98_on_o2_pct=pct(
            sum(1 for s in valid if s.spo2 is not None and s.spo2 > 98 and s.fio2 > ROOM_AIR + 1e-9), nv
        ),
        true_time_in_target_pct=pct(sum(1 for s in trace if in_target(100.0 * s.sa_true, s.fio2)), n),
        true_below_80_pct=pct(sum(1 for s in trace if s.sa_true < 0.80), n),
        mean_fio2=sum(fio2) / n if n else 0.0,
        adjustments_per_min=adjustments / (n / 60.0) if n else 0.0,
        max_rise_60s=max_change_in_window(fio2, 60, +1),
        max_fall_60s=max_change_in_window(fio2, 60, -1),
        hold_s=sum(1 for s in trace if s.mode == 1),
        fallback_s=sum(1 for s in trace if s.mode == 2),
    )
