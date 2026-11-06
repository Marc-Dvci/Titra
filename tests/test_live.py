"""The live bench's pieces that run without a guest: event records, the judge, and closed-loop summaries."""

import asyncio

import pytest

from bench import live
from bench.frames import BlFrame
from bench.live import CotTwin, Link, Recorder, closed_loop, first, judge, last_fio2_before


def test_a_restart_nobody_asked_for_is_reported() -> None:
    before = {"controller-1": 2.9, "controller-2": 3.0, "model": 41}
    after = {"controller-1": 325.8, "controller-2": 3.0, "model": 41}
    assert live.unprompted_restarts(before, after, "warm-up") == [{"process": "controller-1", "during": "warm-up"}]


def test_a_cases_own_restart_is_not_reported() -> None:
    before = {"controller-2": 3.0, "model": 41}
    after = {"controller-2": 700.0, "model": 99}
    assert live.unprompted_restarts(before, after, "F7", ("controller-2",)) == [{"process": "model", "during": "F7"}]


def test_a_look_that_got_no_answer_reports_nothing() -> None:
    assert (
        live.unprompted_restarts({"controller-1": 2.9, "model": None}, {"controller-1": None, "model": 41}, "F1") == []
    )


def test_only_the_faults_that_restart_a_process_expect_it() -> None:
    assert live.on_purpose({"fault": "kill_titractl", "cot": 2}) == ("controller-2",)
    assert live.on_purpose({"fault": "stop_llm", "cot": 0}) == ("model",)
    assert live.on_purpose({"fault": "cpu_load", "cot": 0}) == ()


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


def test_the_models_escape_attempts_are_added_up() -> None:
    output = "\n".join(
        [
            '{"attempt":"open_blender_driver_1","attempts":100,"refused":100,"errno":"Operation not permitted"}',
            '{"attempt":"raise_priority_to_30","attempts":100,"refused":99,"errno":"Operation not permitted"}',
            '{"allowed":1}',
        ]
    )
    m = live.model_measures("model_escape", output)
    assert m["attempts"] == 200
    assert m["refused"] == 199
    assert m["refused_fraction"] == 199 / 200
    assert m["by_attempt"]["raise_priority_to_30"] == "Operation not permitted"
    assert not live.judge({"refused_fraction": 1.0}, m)


def test_a_full_refusal_passes_and_no_output_fails() -> None:
    output = '{"attempt":"open_a_socket","attempts":100,"refused":100,"errno":"Operation not permitted"}'
    assert live.judge({"refused_fraction": 1.0}, live.model_measures("model_escape", output))
    assert not live.judge({"refused_fraction": 1.0}, live.model_measures("model_escape", "not json"))


def test_the_model_saturation_needs_real_tokens() -> None:
    m = live.model_measures("model_saturated", '{"seconds":600,"requests":12,"failed":0,"tokens":3000}')
    assert m["model_generated_tokens"] == 3000
    assert live.judge({"model_generated_tokens": 1000}, m)
    assert not live.judge({"model_generated_tokens": 5000}, m)
    assert not live.judge({"model_generated_tokens": 1000}, {})


def test_the_injection_case_reads_its_last_line() -> None:
    output = (
        '{"malformed_attempts":11,"malformed_rejected":11}\n'
        '{"injection_ran":1,"injected_answer_passed":1,"answer_contains_the_setting":0,"attempts":12,"refused":12}'
    )
    m = live.model_measures("injection_and_malformed_output", output)
    assert m["refused_fraction"] == 1.0
    assert m["answer_contains_the_setting"] == 0
    assert live.judge({"refused_fraction": 1.0}, m)


def test_an_urgent_request_must_meet_its_deadline_and_preempt() -> None:
    good = live.model_measures(
        "urgent_during_long_request",
        '{"urgent_total_ms":9000,"urgent_deadline_met":1,"long_preempted":40,"long_total_ms":70000}',
    )
    assert live.judge({"urgent_deadline_met": True, "long_preempted": True}, good)
    late = live.model_measures("urgent_during_long_request", '{"urgent_deadline_met":0,"long_preempted":40}')
    assert not live.judge({"urgent_deadline_met": True}, late)
    none = live.model_measures("urgent_during_long_request", '{"urgent_deadline_met":1,"long_preempted":0}')
    assert not live.judge({"long_preempted": True}, none)


def test_a_client_must_not_lift_the_servers_priority() -> None:
    ok = live.model_measures("high_priority_client", '{"client_priority":20,"receive_priority":6}')
    assert live.judge({"receive_priority": 6}, ok)
    lifted = live.model_measures("high_priority_client", '{"client_priority":20,"receive_priority":20}')
    assert not live.judge({"receive_priority": 6}, lifted)


def test_the_restart_budget_needs_the_page_to_have_said_unavailable() -> None:
    assert live.judge({"advice_unavailable_seen": True}, {"advice_unavailable_seen": True})
    assert not live.judge({"advice_unavailable_seen": True}, {"advice_unavailable_seen": False})
    assert not live.judge({"advice_unavailable_seen": True}, {})
