"""The simulated oximeter and the bench's oximeter faults."""

import random

import pytest

from bench.frames import INTERFERENCE, SENSOR_OFF, LineSplitter, parse_ox
from bench.oximeter import AVERAGING_S, Oximeter, OximeterParams

QUIET = OximeterParams(0.0, 0.0, 0.0)


def ox(seed: int = 1, p: OximeterParams = QUIET) -> Oximeter:
    return Oximeter(random.Random(seed), p)


def frames(o: Oximeter, n: int, sa: float = 0.93) -> list[bytes]:
    return [o.transmit(sa, 155.0) for _ in range(n)]


def test_clock_advances_one_per_frame() -> None:
    o = ox()
    start = o.clock
    f = [parse_ox(b.decode().rstrip("\r\n")) for b in frames(o, 5)]
    assert [x.clock for x in f] == [start + i for i in range(1, 6)]


def test_reading_is_an_average_of_the_last_eight_seconds() -> None:
    o = ox()
    o._avg.extend([0.80] * AVERAGING_S)
    spo2 = [o.measure(0.99, 155.0).spo2 for _ in range(AVERAGING_S)]
    assert spo2[0] is not None and spo2[-1] is not None
    assert spo2[0] < 85 and spo2[-1] > 95


def test_fixed_spo2_reports_the_set_value() -> None:
    o = ox()
    o.fixed_spo2 = 80
    assert {o.measure(0.99, 155.0).spo2 for _ in range(20)} == {80}


def test_natural_sensor_off_episode_sends_no_values() -> None:
    o = ox(2, OximeterParams(0.0, 0.0, 3600.0, (30.0, 30.0)))
    f = o.measure(0.93, 155.0)
    assert (f.spo2, f.pr, f.pi_centi, f.flags) == (None, None, None, SENSOR_OFF)


def test_flagged_artefact_lowers_the_reading_with_the_interference_flag() -> None:
    o = ox(3, OximeterParams(3600.0, 0.0, 0.0))
    readings = [o.measure(0.95, 155.0) for _ in range(5)]
    assert all(r.flags == INTERFERENCE for r in readings)
    assert all(r.spo2 is not None and r.spo2 < 92 for r in readings)


def test_unflagged_artefact_lowers_the_reading_without_a_flag() -> None:
    o = ox(4, OximeterParams(0.0, 3600.0, 0.0))
    readings = [o.measure(0.95, 155.0) for _ in range(5)]
    assert all(r.flags == 0 for r in readings)
    assert all(r.spo2 is not None and r.spo2 < 92 for r in readings)


def test_fault_cable_cut_sends_nothing() -> None:
    o = ox()
    frames(o, 3)
    o.force("cable_cut")
    assert frames(o, 5) == [b""] * 5


def test_fault_sensor_off_sends_flagged_frames() -> None:
    o = ox()
    o.force("sensor_off")
    f = parse_ox(o.transmit(0.93, 155.0).decode().rstrip("\r\n"))
    assert f.flags == SENSOR_OFF and f.spo2 is None


def test_fault_repeat_frame_resends_the_last_frame_byte_for_byte() -> None:
    o = ox()
    last = frames(o, 3)[-1]
    o.force("repeat_frame")
    assert frames(o, 4) == [last] * 4


def test_fault_frozen_values_keep_the_values_while_the_clock_advances() -> None:
    o = ox()
    last = parse_ox(frames(o, 3)[-1].decode().rstrip("\r\n"))
    o.force("frozen_values")
    f = [parse_ox(b.decode().rstrip("\r\n")) for b in frames(o, 25, sa=0.70)]
    assert {(x.spo2, x.pr, x.pi_centi) for x in f} == {(last.spo2, last.pr, last.pi_centi)}
    assert [x.clock for x in f] == [last.clock + i for i in range(1, 26)]


def test_fault_garbage_adds_noise_and_corrupts_every_third_frame() -> None:
    o = ox()
    o.force("garbage_line")
    data = frames(o, 30)
    assert all(len(d) > 40 for d in data)
    good = 0
    for d in data:
        for line in LineSplitter().feed(d):
            try:
                parse_ox(line)
                good += 1
            except ValueError:
                pass
    assert good == 20


def test_ending_a_fault_restores_normal_frames() -> None:
    o = ox()
    o.force("cable_cut")
    frames(o, 3)
    o.force(None)
    assert parse_ox(o.transmit(0.93, 155.0).decode().rstrip("\r\n")).spo2 == pytest.approx(93, abs=4)
