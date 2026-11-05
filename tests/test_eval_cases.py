"""The committed case set agrees with the spec, and the quoted figures agree with their sources' abstracts."""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = (ROOT / "docs" / "SPEC.md").read_text(encoding="utf-8")


def load(name: str) -> dict:  # type: ignore[type-arg]
    data: dict = json.loads((ROOT / "eval" / name).read_text(encoding="utf-8"))  # type: ignore[type-arg]
    return data


def test_fault_matrix_has_the_fourteen_cases_of_the_spec() -> None:
    cases = load("fault_matrix.json")["cases"]
    assert [c["id"] for c in cases] == [f"F{i}" for i in range(1, 15)]
    for c in cases:
        assert f"| {c['id']} |" in SPEC


def test_fault_budgets_match_the_spec_table() -> None:
    cases = {c["id"]: c["expect"] for c in load("fault_matrix.json")["cases"]}
    rows = {m.group(1): m.group(2) for m in re.finditer(r"^\| (F\d+) \|.*\| ([^|]*) \|$", SPEC, re.M)}
    for cid, expect in cases.items():
        for key, unit in (
            ("alarm_within_s", "Alarm"),
            ("fallback_within_s", "fallback"),
            ("resume_within_s", "resume"),
        ):
            if key in expect:
                assert f"{unit} {expect[key]} s".lower() in rows[cid].lower(), (cid, key)


def test_missed_tick_definition_matches_the_spec() -> None:
    assert load("fault_matrix.json")["missed_tick_s"] == 1.5
    assert "longer than 1.5 s" in SPEC


def test_trial_figures_match_the_published_abstract() -> None:
    t = load("reference.json")["trial"]
    assert (t["time_in_target_manual_pct"], t["time_in_target_automated_pct"]) == (30.1, 88.1)
    assert t["adjusted_mean_difference_pct"] == 58.0 and t["difference_ci95_pct"] == [48.0, 67.9]
    assert t["infants_enrolled"] == 49
    for figure in ("30.1%", "88.1%", "58.0", "48.0-67.9", "49 preterm"):
        assert figure in SPEC


def test_bench_figures_match_the_published_abstract() -> None:
    b = load("reference.json")["bench"]
    assert b["max_sustained_fio2_escalation_per_min"] == [0.30, 0.20, 0.20, 0.10, 0.062, 0.30]
    assert b["max_sustained_fio2_reduction_per_min"] == [1.20, 0.20, 0.20, 0.13, 0.53, 1.32]
    assert b["closed_loop_time_in_target_pct"] == [51, 52, 55, 77, 64, 53]
    assert "0.062 to 0.30" in SPEC and "51% to 77%" in SPEC


def test_step_tests_hold_spo2_at_80_and_98() -> None:
    tests = load("step_tests.json")["tests"]
    assert [t["spo2"] for t in tests] == [80, 98]


def test_spec_safety_times_match_the_core_defaults() -> None:
    header = (ROOT / "core" / "src" / "control.c").read_text(encoding="utf-8")
    assert "p.hold_s = 30.0;" in header and "p.stale_s = 3.0;" in header
    assert "p.baseline_window_s = 300.0;" in header and "p.resume_valid = 5;" in header
    sim = (ROOT / "core" / "src" / "sim.c").read_text(encoding="utf-8")
    assert "#define LINK_HOLD_S 3.0" in sim and "#define LINK_FALLBACK_S 15.0" in sim
    assert "30 s from the last valid sample" in SPEC and "15 s from the last command" in SPEC
