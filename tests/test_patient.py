"""The simulated baby's physiology (docs/MODEL.md)."""

import random

import pytest
from hypothesis import given
from hypothesis import strategies as st

from bench.patient import (
    CIRCULATION_DELAY_S,
    Patient,
    PatientParams,
    arterial_saturation,
    content,
    fio2_for_saturation,
    lag,
    neonatal_saturation,
    saturation_for_content,
    severinghaus,
)


def params(**kw: float) -> PatientParams:
    base = dict(
        shunt=0.15,
        vq_deficit_mmHg=50.0,
        paco2_mmHg=50.0,
        apnoea_per_h=0.0,
        apnoea_duration_s=(10.0, 20.0),
        shunt_episodes_per_h=0.0,
        shunt_episode_size=(0.05, 0.15),
        shunt_episode_minutes=(5.0, 20.0),
    )
    base.update(kw)
    return PatientParams(**base)  # type: ignore[arg-type]


def test_severinghaus_is_half_saturated_at_the_adult_p50() -> None:
    assert severinghaus(26.8) == pytest.approx(0.5, abs=0.01)


def test_neonatal_curve_is_half_saturated_at_20_mmhg() -> None:
    assert neonatal_saturation(20.0) == pytest.approx(0.5, abs=0.01)
    assert neonatal_saturation(0.0) == 0.0


@given(st.floats(0.0, 700.0), st.floats(0.0, 700.0))
def test_saturation_and_content_rise_with_tension(a: float, b: float) -> None:
    lo, hi = sorted((a, b))
    assert neonatal_saturation(lo) <= neonatal_saturation(hi)
    assert content(lo) <= content(hi)


@given(st.floats(1.0, 600.0))
def test_saturation_for_content_inverts_content(p: float) -> None:
    assert saturation_for_content(content(p)) == pytest.approx(neonatal_saturation(p), abs=1e-6)


def test_saturation_for_content_bounds() -> None:
    assert saturation_for_content(0.0) == 0.0
    assert saturation_for_content(1000.0) == pytest.approx(neonatal_saturation(760.0))


@given(st.floats(0.21, 1.0), st.floats(0.21, 1.0), st.floats(0.0, 0.4), st.floats(0.0, 120.0))
def test_more_oxygen_never_lowers_saturation(f1: float, f2: float, shunt: float, vq: float) -> None:
    lo, hi = sorted((f1, f2))
    assert arterial_saturation(lo, shunt, vq, 50.0) <= arterial_saturation(hi, shunt, vq, 50.0) + 1e-9


@given(st.floats(0.21, 1.0), st.floats(0.0, 0.4), st.floats(0.0, 0.4))
def test_more_shunt_never_raises_saturation(f: float, s1: float, s2: float) -> None:
    lo, hi = sorted((s1, s2))
    assert arterial_saturation(f, hi, 50.0, 50.0) <= arterial_saturation(f, lo, 50.0, 50.0) + 1e-9


@given(st.floats(0.05, 0.25), st.floats(10.0, 100.0), st.floats(45.0, 55.0))
def test_fio2_for_saturation_finds_the_fio2_for_93_percent(shunt: float, vq: float, paco2: float) -> None:
    f = fio2_for_saturation(0.93, shunt, vq, paco2)
    assert 0.21 <= f <= 1.0
    if 0.21 < f < 1.0:
        assert arterial_saturation(f, shunt, vq, paco2) == pytest.approx(0.93, abs=1e-6)


def test_fio2_for_saturation_at_the_edges() -> None:
    assert fio2_for_saturation(0.93, 0.0, 0.0, 40.0) == 0.21
    assert fio2_for_saturation(0.999, 0.6, 150.0, 55.0) == 1.0


def test_lag_moves_toward_target_by_the_exact_fraction() -> None:
    assert lag(0.0, 1.0, 1.0, 1.0) == pytest.approx(1 - 0.36787944, abs=1e-6)
    assert lag(0.5, 0.5, 3.0, 1.0) == 0.5


def test_saturation_reaches_the_sensor_after_the_circulation_delay() -> None:
    p = Patient(params(shunt=0.10, vq_deficit_mmHg=30.0), random.Random(1), 0.30)
    first = p.step(0.30)
    for _ in range(200):
        p.step(0.30)
    steady = p.sa
    p2 = Patient(params(shunt=0.10, vq_deficit_mmHg=30.0), random.Random(1), 0.30)
    seen = [p2.step(0.21) for _ in range(CIRCULATION_DELAY_S + 1)]
    assert seen[:CIRCULATION_DELAY_S] == [first] * CIRCULATION_DELAY_S
    assert seen[CIRCULATION_DELAY_S] < first
    assert steady == pytest.approx(first, abs=0.02)


def test_apnoea_desaturates_and_breathing_recovers() -> None:
    # At the FiO2 this baby needs for about 93%, an apnoea takes saturation down the steep part of the curve.
    p = Patient(params(apnoea_per_h=3600.0, apnoea_duration_s=(20.0, 20.0)), random.Random(3), 0.25)
    start = p.step(0.25)
    lows = [p.step(0.25) for _ in range(30)]
    assert min(lows) < start - 0.05
    p.p = params(apnoea_per_h=0.0)
    for _ in range(300):
        end = p.step(0.25)
    assert end == pytest.approx(start, abs=0.03)


def test_pulse_rate_falls_during_a_long_apnoea() -> None:
    p = Patient(params(apnoea_per_h=3600.0, apnoea_duration_s=(25.0, 25.0)), random.Random(4), 0.30)
    base = p.pr_base
    for _ in range(20):
        p.step(0.30)
    assert p.pr < base - 30.0


def test_worsening_ramp_raises_the_mean_shunt() -> None:
    p = Patient(params(shunt_ramp=0.10, duration_s=1000.0), random.Random(5), 0.30)
    assert p.mean_shunt() == pytest.approx(0.15)
    for _ in range(1000):
        p.step(0.30)
    assert p.mean_shunt() == pytest.approx(0.25)


def test_shunt_episode_adds_shunt_then_ends() -> None:
    p = Patient(
        params(shunt_episodes_per_h=3600.0, shunt_episode_size=(0.1, 0.1), shunt_episode_minutes=(1.0, 1.0)),
        random.Random(6),
        0.30,
    )
    p.step(0.30)
    p.p = params(shunt_episodes_per_h=0.0)
    for _ in range(60):
        p.step(0.30)
    assert p.episode_shunt() == pytest.approx(0.1, abs=0.02)
    for _ in range(200):
        p.step(0.30)
    assert p.episode_shunt() == 0.0


def test_same_seed_same_baby() -> None:
    a = Patient(params(apnoea_per_h=20.0, shunt_episodes_per_h=2.0), random.Random(7), 0.3)
    b = Patient(params(apnoea_per_h=20.0, shunt_episodes_per_h=2.0), random.Random(7), 0.3)
    assert [a.step(0.3) for _ in range(500)] == [b.step(0.3) for _ in range(500)]
