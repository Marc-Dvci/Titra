"""Closed-loop runs, step tests and the command line, end to end on short scenarios."""

import dataclasses
import json
from pathlib import Path

import pytest

from bench import evaluate
from bench.__main__ import main
from bench.scenario import draw
from bench.simrun import run
from bench.steptest import STEP_TESTS, run_all

pytestmark = pytest.mark.integration


def short(family: str, seed: int, seconds: int = 1800):  # type: ignore[no-untyped-def]
    s = draw(family, seed)
    return dataclasses.replace(s, duration_s=seconds)


def test_a_run_is_deterministic() -> None:
    a = run(short("moderate", 1), "titra")
    b = run(short("moderate", 1), "titra")
    assert a.trace == b.trace


def test_every_controller_faces_the_same_baby() -> None:
    sat = {c: [x.sa_true for x in run(short("unstable", 2, 30), c).trace] for c in ("titra", "fixed", "manual")}
    # The first seconds are identical: the baby has not yet reacted to any difference in FiO2.
    assert sat["titra"][:5] == sat["fixed"][:5] == sat["manual"][:5]


def test_fixed_reference_never_changes_fio2() -> None:
    r = run(short("unstable", 3), "fixed")
    assert {x.fio2 for x in r.trace} == {r.scenario.initial_fio2}
    assert r.summary.adjustments_per_min == 0.0


def test_titra_raises_fio2_for_a_worsening_baby() -> None:
    s = draw("worsening", 4)
    r = run(s, "titra")
    first = sum(x.fio2 for x in r.trace[:1800]) / 1800
    last = sum(x.fio2 for x in r.trace[-1800:]) / 1800
    assert last > first + 0.03


def test_unknown_controller_is_refused() -> None:
    with pytest.raises(ValueError):
        run(short("stable", 0, 10), "autopilot")


def test_step_tests_follow_the_published_protocol() -> None:
    cfg = json.loads(STEP_TESTS.read_text(encoding="utf-8"))
    assert [(t["spo2"], t["hold_s"]) for t in cfg["tests"]] == [(80, 600), (98, 600)]
    hypo, hyper = run_all()
    assert hypo.commanded_per_min > 0.0 and hyper.commanded_per_min > 0.0
    assert hypo.commanded[-1] > hypo.commanded[0]
    assert hyper.commanded[-1] < hyper.commanded[0]
    assert len(hypo.commanded) == 600


def test_cli_sim_writes_a_trace(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    out = tmp_path / "trace.csv"
    assert main(["sim", "stable", "0", "--controller", "fixed", "--csv", str(out)]) == 0
    assert "time_in_target_pct" in capsys.readouterr().out
    lines = out.read_text(encoding="utf-8").splitlines()
    assert lines[0].startswith("t,sa_true,spo2")
    assert len(lines) == draw("stable", 0).duration_s + 1


def test_cli_steps_prints_both_tests(capsys: pytest.CaptureFixture[str]) -> None:
    assert main(["steps"]) == 0
    out = capsys.readouterr().out
    assert "S1 hypoxaemia" in out and "S2 hyperoxaemia" in out


def test_tuned_params_load_when_present() -> None:
    params = evaluate.tuned_params()
    assert isinstance(params, dict)
    for value in params.values():
        assert value > 0.0
