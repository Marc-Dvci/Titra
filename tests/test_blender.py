"""The simulated blender and its own watchdog (docs/SPEC.md, "Safety layers")."""

import pytest

from bench.blender import FALLBACK_AFTER_S, HOLD_AFTER_S, SERVO_RATE_PER_S, Blender
from bench.frames import BlFrame


def cmd(b: Blender, t: float, fio2: float, alarm: int = 0, backup: float = 0.30) -> None:
    b.on_frame(t, BlFrame(int(t) & 0xFFFF, int(round(fio2 * 1000)), int(round(backup * 1000)), alarm))


def test_watchdog_constants_match_the_spec() -> None:
    assert (HOLD_AFTER_S, FALLBACK_AFTER_S) == (3.0, 15.0)


def test_servo_moves_at_its_rate_limit() -> None:
    b = Blender(backup_fio2=0.30, fio2=0.30)
    cmd(b, 0.0, 0.60)
    b.step(1.0)
    assert b.fio2 == pytest.approx(0.30 + SERVO_RATE_PER_S)


def test_circuit_fio2_follows_with_a_lag() -> None:
    b = Blender(backup_fio2=0.30, fio2=0.30)
    for t in range(1, 60):
        cmd(b, t - 0.5, 0.40)
        c = b.step(float(t))
        if t == 3:
            assert 0.30 < c < 0.40
    assert c == pytest.approx(0.40, abs=0.002)


def test_holds_after_3_s_and_falls_back_after_15_s() -> None:
    b = Blender(backup_fio2=0.30, fio2=0.45)
    for t in range(1, 301):
        cmd(b, float(t), 0.45)
        b.step(float(t))
    last = 300.0
    b.step(last + 2.9)
    assert b.state == "N"
    b.step(last + 3.0)
    assert b.state == "H" and b.alarm_sounding
    b.step(last + 14.9)
    assert b.state == "H"
    b.step(last + 15.0)
    assert b.state == "F"
    assert b.fallback_fio2 == pytest.approx(0.45)
    assert [e.kind for e in b.events[-2:]] == ["hold", "fallback"]


def test_fallback_never_sits_below_the_backup() -> None:
    b = Blender(backup_fio2=0.30, fio2=0.25)
    for t in range(1, 100):
        cmd(b, float(t), 0.25)
    b.step(200.0)
    assert b.state == "F" and b.fallback_fio2 == pytest.approx(0.30)


def test_fallback_uses_the_lower_median_of_the_last_five_minutes() -> None:
    b = Blender(backup_fio2=0.21, fio2=0.30)
    for t in range(1, 200):
        cmd(b, float(t), 0.80, backup=0.21)
    for t in range(200, 501):
        cmd(b, float(t), 0.40, backup=0.21)
    b.step(520.0)
    assert b.fallback_fio2 == pytest.approx(0.40)


def test_a_new_frame_resumes_normal_operation() -> None:
    b = Blender(backup_fio2=0.30, fio2=0.40)
    b.step(20.0)
    assert b.state == "F"
    cmd(b, 21.0, 0.35)
    assert b.state == "N" and b.events[-2].kind == "resume"


def test_alarm_in_the_frame_sounds_at_the_cot() -> None:
    b = Blender(backup_fio2=0.30, fio2=0.40)
    cmd(b, 1.0, 0.40, alarm=2)
    assert b.alarm_sounding
    cmd(b, 2.0, 0.40, alarm=1)
    assert not b.alarm_sounding


def test_status_reports_the_circuit_fio2_and_state() -> None:
    b = Blender(backup_fio2=0.30, fio2=0.40)
    cmd(b, 1.0, 0.40)
    b.step(1.5)
    s = b.status()
    assert (s.fio2_pm, s.state) == (400, "N")
