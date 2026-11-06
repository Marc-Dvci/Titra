"""The advisor's flags through the C core, and their scoring against ground truth."""

import ctypes
import json
from pathlib import Path

import pytest

from bench import flags
from bench.metrics import Second

ROOT = Path(__file__).resolve().parents[1]


def trace(n: int, spo2: int = 93, fio2: float = 0.30, sa: float = 0.93) -> list[Second]:
    return [Second(float(i), sa, spo2, 0, fio2, fio2, 0, 0) for i in range(n)]


def test_the_bench_and_the_core_agree_on_the_structures() -> None:
    assert flags.default_thresholds().rising_delta == pytest.approx(0.03)


def test_a_calm_window_through_the_core_raises_nothing() -> None:
    f = flags.compute(flags.to_seconds(trace(10800)), 10800, 10800)
    assert f.flags == 0
    assert f.time_in_target_pct == 100
    assert flags.flag_names(f.flags) == []


def test_a_rise_in_need_raises_its_flag() -> None:
    t = trace(7200, fio2=0.30) + trace(3600, fio2=0.35)
    f = flags.compute(flags.to_seconds(t), 10800, 10800)
    assert "rising_need" in flags.flag_names(f.flags)
    assert f.fio2_late_pm == 350


def test_the_window_is_the_last_seconds_before_the_end() -> None:
    t = trace(3600, spo2=70) + trace(3600, spo2=93)
    calm = flags.compute(flags.to_seconds(t), 7200, 3600)
    assert calm.time_in_target_pct == 100
    assert calm.desat_episodes == 0
    both = flags.compute(flags.to_seconds(t), 7200, 7200)
    assert both.time_in_target_pct == 50


def test_a_missing_reading_counts_as_no_valid_sample() -> None:
    t = trace(100)
    t[10:20] = [Second(float(i), 0.93, None, 0, 0.30, 0.30, 0, 0) for i in range(10, 20)]
    seconds = flags.to_seconds(t)
    assert seconds[15].spo2 == -1
    assert seconds[15].status == flags.SAMPLE_INVALID
    assert flags.compute(seconds, 100, 100).probe_events == 1


def test_a_flagged_reading_counts_as_no_valid_sample() -> None:
    t = trace(100)
    t[10:20] = [Second(float(i), 0.93, 93, 1, 0.30, 0.30, 0, 0) for i in range(10, 20)]
    assert flags.compute(flags.to_seconds(t), 100, 100).valid_seconds == 90


def test_true_dips_count_from_the_minimum_length() -> None:
    t = trace(3600)
    t[100:109] = [Second(float(i), 0.80, 80, 0, 0.3, 0.3, 0, 0) for i in range(100, 109)]
    assert flags.true_dips_per_h(t, 0.85, 10) == 0.0
    t[100:110] = [Second(float(i), 0.80, 80, 0, 0.3, 0.3, 0, 0) for i in range(100, 110)]
    assert flags.true_dips_per_h(t, 0.85, 10) == 1.0
    assert flags.true_dips_per_h([], 0.85, 10) == 0.0


def row(family: str, raised: list[str], expect: list[str], allow: list[str]) -> dict[str, object]:
    return {"family": family, "end_s": 10800, "raised": raised, "expect": expect, "allow": allow}


def test_scoring_counts_recall_and_false_alarms() -> None:
    rows = [
        row("worsening", ["rising_need"], ["rising_need"], []),
        row("worsening", [], ["rising_need"], []),
        row("stable", ["probe_trouble"], [], []),
        row("stable", [], [], []),
    ]
    s = flags.score(rows, 10800)
    assert s["recall"]["rising_need"] == 0.5
    assert s["stable_windows"] == 2
    assert s["stable_windows_with_a_flag"] == 1
    assert s["windows_with_a_false_alarm"] == 1


def test_an_allowed_flag_is_not_a_false_alarm() -> None:
    s = flags.score([row("unstable", ["high_need"], [], ["high_need"])], 10800)
    assert s["windows_with_a_false_alarm"] == 0


def test_the_cases_cover_every_scored_family_and_seed_set() -> None:
    cfg = json.loads((ROOT / "eval" / "flag_cases.json").read_text(encoding="utf-8"))
    families = {c["family"] for c in cfg["cases"]}
    assert {"stable", "worsening", "improving", "artefact_heavy", "unstable"} <= families
    assert not set(cfg["seeds"]["development"]) & set(cfg["seeds"]["held_out"])


def test_the_committed_held_out_result_meets_the_targets() -> None:
    summary = json.loads((ROOT / "eval" / "results" / "flags_held_out.json").read_text(encoding="utf-8"))["summary"]
    assert all(v >= 0.8 for v in summary["recall"].values())
    # Any flag raised on a stable baby in this result is a dip the simulator confirms, never a false alarm.
    stable = [v for k, v in summary["per_case"].items() if k.startswith("stable")]
    assert sum(c["false_alarm"] for c in stable) == 0


@pytest.mark.parametrize("bit, name", list(flags.FLAGS.items()))
def test_flag_bits_match_the_core(bit: int, name: str) -> None:
    from bench.core import lib

    fn = lib().tf_flag_name
    fn.restype = ctypes.c_char_p
    fn.argtypes = [ctypes.c_uint32]
    assert fn(bit).decode() == name


@pytest.mark.integration
def test_a_small_case_set_runs_from_the_simulator_to_a_result_file(tmp_path: Path) -> None:
    cfg = json.loads((ROOT / "eval" / "flag_cases.json").read_text(encoding="utf-8"))
    cfg["cases"] = [c for c in cfg["cases"] if c["family"] == "stable"][:1]
    cfg["seeds"] = {"development": [0], "held_out": [1000]}
    path = tmp_path / "cases.json"
    path.write_text(json.dumps(cfg), encoding="utf-8")
    result = flags.write_results("development", tmp_path, path)
    assert result["summary"]["windows"] == 1
    assert (tmp_path / "flags_development.json").exists()
    assert flags.facts_dict(flags.compute(flags.to_seconds(trace(100)), 100, 100))["seconds"] == 100
