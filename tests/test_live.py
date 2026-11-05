"""The live bench's pieces that run without a guest: event records, the judge, and closed-loop summaries."""

import asyncio

import pytest

from bench.frames import BlFrame
from bench.live import CotTwin, Link, Recorder, closed_loop, first, judge, last_fio2_before


def test_recorder_filters_by_time_cot_and_kind() -> None:
    rec = Recorder()
    a = rec.add(1, "bl", fio2=0.3, alarm=0)
    rec.add(2, "bl", fio2=0.4, alarm=0)
    rec.add(1, "ox_connect")
    assert rec.since(a.t, 1, "bl")[0] is a
    assert len(rec.since(0.0, 1)) == 2
    assert len(rec.since(0.0, kind="bl")) == 2


def test_first_and_last_fio2_before() -> None:
    rec = Recorder()
    e1 = rec.add(2, "bl", fio2=0.31, alarm=0)
    rec.add(2, "bl", fio2=0.33, alarm=2)
    assert first(rec.events, lambda e: e.data.get("alarm") == 2) is rec.events[1]
    assert first(rec.events, lambda e: e.kind == "nothing") is None
    assert last_fio2_before(rec, 2, rec.events[1].t + 1.0) == pytest.approx(0.33)
    assert last_fio2_before(rec, 2, e1.t) is None
    assert last_fio2_before(rec, 3, 100.0) is None


@pytest.mark.parametrize(
    ("expect", "measured", "ok"),
    [
        ({"alarm_within_s": 2}, {"alarm_s": 1.0}, True),
        ({"alarm_within_s": 2}, {"alarm_s": 2.5}, False),
        ({"alarm_within_s": 2}, {}, False),
        ({"fallback_within_s": 32}, {"fallback_s": 29.96}, True),
        ({"fallback_within_s": 5}, {"fallback_s": 5.2}, False),
        ({"resume_within_s": 10}, {"resume_s": 1.2}, True),
        ({"fio2_unchanged_while_holding": True}, {"fio2_unchanged_while_holding": False}, False),
        ({"no_fallback": True}, {"fallback_seen": False}, True),
        ({"no_fallback": True}, {"fallback_seen": True}, False),
        ({"no_step_at_resume": True}, {"step_at_resume": 0.01}, True),
        ({"no_step_at_resume": True}, {"step_at_resume": 0.02}, False),
        ({"refused_fraction": 1.0}, {"refused_fraction": 1.0}, True),
        ({"refused_fraction": 1.0}, {"refused_fraction": 0.99}, False),
        ({"missed_ticks": 0}, {"late_ticks": {1: 0, 2: 0}}, True),
        ({"missed_ticks": 0}, {"late_ticks": {1: 0, 2: 1}}, False),
        ({"missed_ticks": 0}, {}, False),
        ({"alarm_if_gap_over_s": 3}, {"gap_s": 1.2}, True),
        ({"alarm_if_gap_over_s": 3}, {"gap_s": 4.0}, False),
        ({"alarm_if_gap_over_s": 3}, {"gap_s": 4.0, "alarm_s": 3.1}, True),
        ({"no_crash": True}, {}, True),
    ],
)
def test_judge_applies_each_budget(expect: dict, measured: dict, ok: bool) -> None:  # type: ignore[type-arg]
    assert judge(expect, measured) is ok


def test_cot_twin_answers_a_command_frame_and_records_it() -> None:
    rec = Recorder()
    twin = CotTwin(2, rec)
    sent: list[bytes] = []
    twin.bl_link.write = sent.append  # type: ignore[method-assign]
    twin._bl_data(b"$BL,7,330,300,2*00\r\n")
    twin._bl_data(BlFrame(7, 330, 300, 2).encode())
    events = [e for e in rec.events if e.cot == 2]
    assert events[0].kind == "bl_bad"
    bl = [e for e in events if e.kind == "bl"]
    assert bl and bl[-1].data == {"fio2": 0.33, "alarm": 2, "seq": 7}
    assert sent and sent[-1].startswith(b"$BS,7,")


def test_cot_twin_tick_sends_an_oximeter_frame_and_a_status() -> None:
    rec = Recorder()
    twin = CotTwin(1, rec)
    ox: list[bytes] = []
    bs: list[bytes] = []
    twin.ox_link.write = ox.append  # type: ignore[method-assign]
    twin.bl_link.write = bs.append  # type: ignore[method-assign]
    twin.tick()
    assert ox and ox[0].startswith(b"$OX,")
    assert bs and bs[0].startswith(b"$BS,")
    assert twin.seconds


def test_link_keeps_the_newest_client_and_refuses_while_cut() -> None:
    async def scenario() -> list[bytes]:
        got: list[bytes] = []
        link = Link(0, got.append, lambda: None)
        server = await asyncio.start_server(link._client, "127.0.0.1", 0)
        port = server.sockets[0].getsockname()[1]
        r1, w1 = await asyncio.open_connection("127.0.0.1", port)
        w1.write(b"one")
        await w1.drain()
        await asyncio.sleep(0.05)
        link.refusing = True
        r2, w2 = await asyncio.open_connection("127.0.0.1", port)
        await asyncio.sleep(0.05)
        refused = await r2.read(10)
        link.refusing = False
        link.drop()
        server.close()
        await server.wait_closed()
        w1.close()
        return [*got, refused]

    out = asyncio.run(scenario())
    assert out[0] == b"one"
    assert out[-1] == b""


def test_closed_loop_counts_valid_seconds_in_the_window_against_the_trial_definition() -> None:
    twin = CotTwin(1, Recorder())
    twin.seconds = [
        (1.0, 93, 0, 0.30),  # in target on oxygen
        (2.0, 99, 0, 0.30),  # above target on oxygen
        (3.0, None, 0, 0.30),  # no displayed value: not counted
        (4.0, 92, 1, 0.30),  # flagged: not counted
        (5.0, 99, 0, 0.21),  # in target in room air
        (50.0, 93, 0, 0.30),  # outside the window
    ]
    out = closed_loop({1: twin}, 0.0, 10.0)["1"]
    assert out["seconds_valid"] == 3
    assert out["time_in_target_pct"] == 66.7
    assert out["scenario"] == f"{twin.scn.family}:{twin.scn.seed}"
