"""The reference controllers (docs/MODEL.md)."""

import random

import pytest

from bench.reference import FixedFio2, ManualCare


def test_fixed_fio2_never_moves() -> None:
    f = FixedFio2(0.27)
    assert {f.step(float(t), s) for t, s in enumerate([50, 93, 100, None])} == {0.27}


def nurse(seed: int = 1, fio2: float = 0.30) -> ManualCare:
    return ManualCare(random.Random(seed), fio2)


def test_nurse_does_nothing_while_spo2_is_in_range() -> None:
    n = nurse()
    for t in range(3000):
        assert n.step(float(t), 93) == 0.30
    assert n.visits == 0


def test_nurse_answers_a_low_alarm_after_a_delay_and_raises_fio2() -> None:
    n = nurse()
    t = 0
    while n.visits == 0 and t < 5000:
        n.step(float(t), 85)
        t += 1
    assert n.visits == 1
    assert t > 10
    assert n.fio2 == pytest.approx(0.35)


def test_nurse_raises_by_ten_points_below_80() -> None:
    n = nurse()
    t = 0
    while n.visits == 0:
        n.step(float(t), 75)
        t += 1
    assert n.fio2 == pytest.approx(0.40)


def test_nurse_weans_on_a_high_alarm_but_not_below_room_air() -> None:
    n = nurse(2, 0.24)
    for t in range(20000):
        n.step(float(t), 99)
    assert n.fio2 == pytest.approx(0.21)


def test_nurse_looks_again_a_minute_later() -> None:
    n = nurse(3)
    t = 0
    while n.visits == 0:
        n.step(float(t), 85)
        t += 1
    first = n.fio2
    for _ in range(61):
        n.step(float(t), 85)
        t += 1
    assert n.fio2 == pytest.approx(first + 0.05)


def test_hourly_round_adjusts_without_an_alarm() -> None:
    n = nurse(4)
    n.step(3599.0, 89)
    assert n.fio2 == 0.30
    n.step(3600.0, 87)
    assert n.fio2 == pytest.approx(0.35)
    assert n.next_round == 7200.0


def test_missing_spo2_changes_nothing() -> None:
    n = nurse(5)
    n.step(3600.0, None)
    assert n.fio2 == 0.30
