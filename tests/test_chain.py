"""The C core's on-board chain, driven through its ctypes API: the safety properties the spec promises.

These run the code the QNX processes run (oximeter framing and quality, controller, blender driver guard and link
watchdog), fed with frames from the simulated oximeter.
"""

import random

import pytest
from hypothesis import given, settings
from hypothesis import strategies as st

from bench.core import A_FALLBACK, A_SIGNAL_HOLD, A_UNTRUSTED, MODE_CONTROL, MODE_FALLBACK, MODE_HOLD, CotChain
from bench.frames import BlFrame, OxFrame, parse_bl

pytestmark = pytest.mark.integration

SLEW = 0.050  # the blender driver's slew limit per command (core/src/sim.c)


def chain(fio2: float = 0.30, backup: float = 0.30, params: dict[str, float] | None = None) -> CotChain:
    return CotChain(0.21, 1.00, backup, fio2, 0.0, params)


def frame(clock: int, spo2: int | None, flags: int = 0, pr: int | None = None, pi: int | None = None) -> bytes:
    pr = (150 + clock % 9) if pr is None and spo2 is not None else pr
    pi = (120 + clock % 13) if pi is None and spo2 is not None else pi
    return OxFrame(clock, spo2, pr, pi, flags).encode()


def tick(c: CotChain, t: float, data: bytes) -> tuple[BlFrame, int, int, int]:
    c.feed(data, t - 0.45)
    bl, out = c.tick(t)
    return parse_bl(bl.decode().rstrip("\r\n")), out.mode, out.alarm, out.alarms


@settings(max_examples=60, deadline=None)
@given(st.lists(st.one_of(st.none(), st.integers(0, 100)), min_size=50, max_size=400), st.floats(0.21, 1.0))
def test_fio2_stays_within_limits_and_slew_on_any_spo2_sequence(seq: list[int | None], start: float) -> None:
    c = chain(start)
    prev = None
    for i, spo2 in enumerate(seq):
        f, mode, _, _ = tick(c, float(i + 1), frame(i + 1, spo2, 0 if spo2 is not None else 0x01))
        fio2 = f.fio2_pm / 1000
        assert 0.21 <= fio2 <= 1.0
        if prev is not None and mode == MODE_CONTROL:
            assert abs(fio2 - prev) <= SLEW + 1e-9
        prev = fio2


@settings(max_examples=40, deadline=None)
@given(st.integers(0, 600), st.floats(0.21, 0.6), st.floats(0.21, 0.6))
def test_fallback_never_sits_below_the_backup(steps: int, start: float, backup: float) -> None:
    c = chain(start, backup)
    rnd = random.Random(steps)
    t = 0.0
    for i in range(steps):
        t += 1.0
        tick(c, t, frame(i + 1, rnd.randint(70, 100)))
    f, mode = BlFrame(0, 0, 0, 0), MODE_CONTROL
    for _ in range(40):
        t += 1.0
        f, mode, _, _ = tick(c, t, b"")
    assert mode == MODE_FALLBACK
    assert f.fio2_pm >= round(backup * 1000) - 1


def test_sensor_off_holds_fio2_alarms_at_once_and_falls_back_within_32_s() -> None:
    c = chain(0.35)
    t = 0.0
    for i in range(300):
        t += 1.0
        f, *_ = tick(c, t, frame(i + 1, 93))
    held = f.fio2_pm
    fault_t = t
    first_alarm = fallback_t = None
    for i in range(300, 360):
        t += 1.0
        f, mode, alarm, alarms = tick(c, t, frame(i + 1, None, 0x01))
        if first_alarm is None and alarms & A_SIGNAL_HOLD:
            first_alarm = t
        if mode == MODE_HOLD:
            assert f.fio2_pm == held
        if fallback_t is None and mode == MODE_FALLBACK:
            fallback_t = t
    assert first_alarm is not None and first_alarm - fault_t <= 2.0
    assert fallback_t is not None and fallback_t - fault_t <= 32.0


def test_cable_cut_alarms_within_5_s_and_falls_back_within_32_s() -> None:
    c = chain(0.35)
    t = 0.0
    for i in range(300):
        t += 1.0
        tick(c, t, frame(i + 1, 93))
    fault_t = t
    first_alarm = fallback_t = None
    for _ in range(60):
        t += 1.0
        _, mode, _, alarms = tick(c, t, b"")
        if first_alarm is None and alarms & A_SIGNAL_HOLD:
            first_alarm = t
        if fallback_t is None and mode == MODE_FALLBACK:
            fallback_t = t
    assert first_alarm is not None and first_alarm - fault_t <= 5.0
    assert fallback_t is not None and fallback_t - fault_t <= 32.0


def test_repeated_frame_falls_back_within_5_s() -> None:
    c = chain(0.35)
    t = 0.0
    last = b""
    for i in range(300):
        t += 1.0
        last = frame(i + 1, 93)
        tick(c, t, last)
    fault_t = t
    for _ in range(10):
        t += 1.0
        _, mode, _, alarms = tick(c, t, last)
        if mode == MODE_FALLBACK:
            assert alarms & A_UNTRUSTED
            break
    assert mode == MODE_FALLBACK and t - fault_t <= 5.0


def test_frozen_values_fall_back_within_22_s_at_the_pre_fault_baseline() -> None:
    c = chain(0.30)
    t = 0.0
    for i in range(300):
        t += 1.0
        tick(c, t, frame(i + 1, 93))
    fault_t = t
    fallback_t = None
    for i in range(300, 330):
        t += 1.0
        # A frozen low reading: the controller escalates until the frozen rule fires.
        f, mode, _, _ = tick(c, t, frame(i + 1, 80, pr=150, pi=120))
        if mode == MODE_FALLBACK and fallback_t is None:
            fallback_t = t
            assert f.fio2_pm == 300
    assert fallback_t is not None and fallback_t - fault_t <= 22.0


def test_garbage_between_frames_and_every_third_frame_corrupt_never_falls_back() -> None:
    c = chain(0.30)
    t = 0.0
    rnd = random.Random(9)
    for i in range(600):
        t += 1.0
        data = frame(i + 1, rnd.randint(90, 96))
        if (i + 1) % 3 == 0:
            data = data[:5] + (b"9" if data[5:6] != b"9" else b"8") + data[6:]
        noise = bytes(rnd.choice(b"0123456789ABCDEF,.*\r#@!") for _ in range(40))
        _, mode, _, _ = tick(c, t, noise + data)
        assert mode != MODE_FALLBACK
    assert c.frames_rejected == 200
    assert c.frames_ok == 400


def test_resume_after_fallback_needs_five_valid_samples_and_causes_no_step() -> None:
    c = chain(0.30)
    t = 0.0
    for i in range(100):
        t += 1.0
        tick(c, t, frame(i + 1, 93))
    for _ in range(40):
        t += 1.0
        f, mode, alarm, alarms = tick(c, t, b"")
    assert mode == MODE_FALLBACK and alarms & A_FALLBACK and alarm == 3
    fb = f.fio2_pm
    modes = []
    for i in range(140, 146):
        t += 1.0
        f, mode, _, _ = tick(c, t, frame(i + 1, 88))
        modes.append(mode)
    assert modes[:4] == [MODE_FALLBACK] * 4 and modes[4] == MODE_CONTROL
    assert abs(f.fio2_pm - fb) <= 50


def test_hypoxaemia_escalates_and_hyperoxaemia_weans() -> None:
    up, down = chain(0.30), chain(0.60)
    t = 0.0
    for i in range(300):
        t += 1.0
        fu, *_ = tick(up, t, frame(i + 1, 82))
        fd, *_ = tick(down, t, frame(i + 1, 99))
    assert fu.fio2_pm > 400
    assert fd.fio2_pm < 600


def test_unknown_controller_parameter_is_refused() -> None:
    with pytest.raises(KeyError):
        chain(params={"no_such_gain": 1.0})
