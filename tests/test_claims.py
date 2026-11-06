"""The figures on the README and in docs/ADVISOR.md are the committed results. Each test fails if a claim stops being true."""

import importlib.util
import json
import re
from pathlib import Path
from typing import Any

import pytest

from bench import model_eval

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "eval" / "results"
README = (ROOT / "README.md").read_text(encoding="utf-8")
ADVISOR = (ROOT / "docs" / "ADVISOR.md").read_text(encoding="utf-8")

_spec = importlib.util.spec_from_file_location("fault_table", ROOT / "scripts" / "fault_table.py")
assert _spec is not None and _spec.loader is not None
fault_table = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fault_table)


def result(name: str) -> dict[str, Any]:
    data: dict[str, Any] = json.loads((RESULTS / name).read_text(encoding="utf-8"))
    return data


def scored(stem: str) -> dict[str, Any]:
    set_name = stem.rsplit("_L", 1)[0]
    windows = model_eval.read_windows(ROOT / "eval" / f"model_windows_{set_name}.txt")
    return model_eval.score_file(RESULTS / f"model_outputs_{stem}.jsonl", windows)


def test_every_fault_case_passed_in_the_committed_run() -> None:
    faults = result("faults.json")
    assert faults["total"] == 23
    assert faults["passed"] == 23
    assert all(c["pass"] for c in faults["cases"])


def test_the_badge_and_the_headline_state_the_fault_count() -> None:
    faults = result("faults.json")
    claim = f"{faults['passed']} of {faults['total']}"
    assert f"Faults on QNX: {claim} within budget" in README
    assert f"{claim} injected faults within budget" in README


def test_the_fault_table_on_the_page_is_the_results_file() -> None:
    faults = result("faults.json")
    for case in faults["cases"]:
        row = f"| {case['id']} | "
        line = next(x for x in README.splitlines() if x.startswith(row))
        assert fault_table.measured(case) in line, case["id"]
        assert line.endswith("| pass |"), case["id"]


def test_the_faults_ran_on_the_target_the_page_names() -> None:
    assert "hardware acceleration" in result("faults.json")["target"]
    assert result("faults.json")["types"] is True


def test_the_model_ran_flat_out_with_no_missed_tick() -> None:
    f17 = next(c for c in result("faults.json")["cases"] if c["id"] == "F17")
    assert all(v == 0 for v in f17["measured"]["late_ticks"].values())
    assert f17["measured"]["model_generated_tokens"] >= 1000


def test_the_urgent_request_really_overtook_the_long_one() -> None:
    f21 = next(c for c in result("faults.json")["cases"] if c["id"] == "F21")
    assert f21["measured"]["urgent_deadline_met"] is True
    assert f21["measured"]["long_preempted"] > 0


def test_the_killed_model_was_reported_on_the_page() -> None:
    f18 = next(c for c in result("faults.json")["cases"] if c["id"] == "F18")
    assert f18["measured"]["advice_unavailable_seen"] is True


@pytest.mark.parametrize(
    ("controller", "label"),
    [("titra", "Titra"), ("fixed", "Fixed FiO2 at the baby's starting need"), ("manual", "Manual-care model")],
)
def test_the_closed_loop_table_is_the_results_files(controller: str, label: str) -> None:
    dev = result("development.json")["mean"][controller]["all"]
    held = result("held_out.json")["mean"][controller]["all"]
    row = (
        f"| {label} | {dev['time_in_target_pct']:.1f}% | {held['time_in_target_pct']:.1f}% | "
        f"{dev['below_80_pct']:.2f}% |"
    )
    assert row in README


@pytest.mark.parametrize(
    ("label", "key", "shown"),
    [
        ("Probe trouble", "probe_trouble", "100%"),
        ("Hold or fallback", "hold_or_fallback", "100%"),
        ("Wean opportunity", "wean_opportunity", "100%"),
        ("Frequent desaturations", "frequent_desaturations", "97.5%"),
        ("Rising need", "rising_need", "90%"),
    ],
)
def test_the_flag_table_is_the_results_file(label: str, key: str, shown: str) -> None:
    summary = result("flags_held_out.json")["summary"]
    assert float(shown.rstrip("%")) == pytest.approx(100 * summary["recall"][key], abs=0.05)
    assert f"| {label} | {shown} | {summary['expected'][key]} |" in ADVISOR


def test_the_flags_false_alarms_are_stated() -> None:
    summary = result("flags_held_out.json")["summary"]
    assert f"{summary['windows_with_a_false_alarm']} of the {summary['windows']} windows" in ADVISOR
    assert f"{summary['stable_windows_with_a_flag']} of the {summary['stable_windows']} stable windows" in ADVISOR


def test_the_forecast_table_is_the_results_file() -> None:
    risk = result("risk_held_out.json")
    model = risk["scorers"]["model"]
    best = max(v["auroc"] for k, v in risk["scorers"].items() if k != "model")
    assert model["auroc"] > best
    assert f"{model['auroc']:.3f}" in README
    assert f"{best:.3f}" in README
    assert (
        f"| {model['auroc']:.3f} | {100 * model['at_alert_rate']['precision']:.1f}% | "
        f"{100 * model['at_alert_rate']['recall']:.1f}% |" in ADVISOR
    )


@pytest.mark.parametrize(("loops", "stem"), [(1, "held_out_L1"), (2, "held_out_L2"), (4, "held_out_L4")])
def test_the_answer_table_is_the_recorded_answers(loops: int, stem: str) -> None:
    s = scored(stem)
    row = (
        f"| {loops} | {s['valid']} of {s['answers']} ({100 * s['valid_rate']:.1f}%) | "
        f"{s['flags_covered']} of {s['flags_raised']} ({100 * s['flag_coverage']:.1f}%) | "
        f"{100 * s['first_is_top_priority_rate']:.1f}% |"
    )
    assert row in ADVISOR


@pytest.mark.parametrize("path", sorted(RESULTS.glob("model_outputs_*.jsonl")), ids=lambda p: p.stem)
def test_no_sentence_from_the_model_holds_a_digit(path: Path) -> None:
    set_name = path.stem.removeprefix("model_outputs_").rsplit("_L", 1)[0]
    windows = model_eval.read_windows(ROOT / "eval" / f"model_windows_{set_name}.txt")
    seen = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        row = json.loads(line)
        if row["validator"] != "ok":
            continue
        facts = {f["cot"]: f for f in windows[row["id"]].facts}
        parsed = json.loads(row["text"])
        for note in parsed.get("notes") or parsed.get("cots"):
            if facts[int(note["cot"])]["flags"]:
                seen += 1
                assert not re.search(r"\d", note.get("why") or note.get("summary") or ""), row["id"]
    assert seen > 0


def test_the_kernel_figures_are_the_results_file() -> None:
    k = result("kernels.json")
    assert k["equivalence"]["differ"] == 0
    assert f"{k['equivalence']['products_compared']:,}" in ADVISOR
    for loops in ("1", "4"):
        b, a = k["before"][loops], k["after"][loops]
        assert f"{b['gen_tok_s']:.2f} to {a['gen_tok_s']:.2f} tok/s" in ADVISOR
        assert f"{b['worst_token_ms']} to {a['worst_token_ms']} ms" in ADVISOR
        assert f"{b['prompt_tok_s']:.1f} to {a['prompt_tok_s']:.1f} tok/s" in ADVISOR
    gen = sorted(100 * (k["after"][x]["gen_tok_s"] / k["before"][x]["gen_tok_s"] - 1) for x in ("1", "4"))
    worst = sorted(100 * (1 - k["after"][x]["worst_token_ms"] / k["before"][x]["worst_token_ms"]) for x in ("1", "4"))
    assert (round(gen[0]), round(gen[1])) == (3, 7)
    assert (round(worst[0]), round(worst[1])) == (14, 15)


@pytest.mark.parametrize(
    ("label", "config"),
    [
        ("1 loop", "dory_1_loop"),
        ("2 loops", "dory_2_loops"),
        ("4 loops", "dory_4_loops"),
        ("Elastic, 30 s deadline", "dory_elastic_30s"),
        ("Elastic, 120 s deadline", "dory_elastic_120s"),
    ],
)
def test_the_timing_table_is_the_boards_results(label: str, config: str) -> None:
    c = result("board.json")["answers_on_the_board"]["held_out"][config]
    assert not any(c["late_ticks_during"])
    row = (
        f"| {label} | {c['gen_tok_s']:.2f} tok/s | {c['prompt_tok_s']:.1f} tok/s | {c['mean_answer_s']:.1f} s | "
        f"{c['longest_answer_s']:.1f} s | {c['deadlines_met']} of {c['answers']} | 0 | "
        f"{max(c['longest_tick_interval_s']):.3f} s |"
    )
    assert row in ADVISOR
