"""Outcome measures (docs/SPEC.md, "Evaluation")."""

import pytest

from bench.metrics import Second, in_target, max_change_in_window, summarise


@pytest.mark.parametrize(
    ("spo2", "fio2", "expected"),
    [
        (90, 0.30, False),
        (91, 0.30, True),
        (95, 0.30, True),
        (96, 0.30, False),
        (90, 0.21, False),
        (91, 0.21, True),
        (100, 0.21, True),
    ],
)
def test_target_uses_the_trial_definition(spo2: int, fio2: float, expected: bool) -> None:
    assert in_target(spo2, fio2) is expected


def test_max_change_in_window_by_direction() -> None:
    v = [0.30] * 10 + [0.30 + 0.01 * i for i in range(30)] + [0.59 - 0.02 * i for i in range(10)]
    assert max_change_in_window(v, 10, +1) == pytest.approx(0.10)
    assert max_change_in_window(v, 10, -1) == pytest.approx(0.18)
    assert max_change_in_window([0.3, 0.3], 5, +1) == 0.0


def sec(t: float, spo2: int | None, fio2: float, flags: int = 0, mode: int = 0, sa: float = 0.93) -> Second:
    return Second(t, sa, spo2, flags, fio2, fio2, mode, 0)


def test_summary_counts_valid_samples_only_for_displayed_measures() -> None:
    trace = [sec(0, 93, 0.30), sec(1, 84, 0.30), sec(2, 79, 0.30), sec(3, 99, 0.30), sec(4, None, 0.30, flags=1)]
    s = summarise(trace)
    assert s.seconds == 5
    assert s.valid_pct == pytest.approx(80.0)
    assert s.time_in_target_pct == pytest.approx(25.0)
    assert s.below_85_pct == pytest.approx(50.0)
    assert s.below_80_pct == pytest.approx(25.0)
    assert s.above_98_on_o2_pct == pytest.approx(25.0)


def test_summary_counts_adjustments_modes_and_true_saturation() -> None:
    trace = [sec(t, 93, 0.30 if t < 60 else 0.31, mode=1 if t % 2 else 2, sa=0.79) for t in range(120)]
    s = summarise(trace)
    assert s.adjustments_per_min == pytest.approx(0.5)
    assert s.hold_s == 60 and s.fallback_s == 60
    assert s.true_below_80_pct == pytest.approx(100.0)
    assert s.true_time_in_target_pct == 0.0
    assert s.max_rise_60s == pytest.approx(0.01)
    assert s.mean_fio2 == pytest.approx(0.305)


def test_summary_of_an_empty_trace_is_zero() -> None:
    s = summarise([])
    assert s.seconds == 0 and s.mean_fio2 == 0.0 and s.adjustments_per_min == 0.0
    assert set(s.as_dict()) >= {"time_in_target_pct", "below_80_pct", "fallback_s"}
