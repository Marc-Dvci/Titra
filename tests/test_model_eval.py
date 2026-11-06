"""Scoring what the on-board model wrote: the same C validator as the board, with no model and no QNX."""

import json
from pathlib import Path

import pytest

from bench import model_eval

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "eval" / "results"


def facts(cot: int, flags: int = 0, priority: int = 0, tir: int = 90) -> dict[str, int]:
    return {
        "cot": cot,
        "seconds": 10800,
        "valid_seconds": 10500,
        "time_in_target_pct": tir,
        "above_95_on_o2_pct": 3,
        "below_85_pct": 1,
        "desat_episodes": 6,
        "longest_desat_s": 48,
        "fio2_early_pm": 250,
        "fio2_late_pm": 340,
        "fio2_max_pm": 360,
        "probe_events": 7,
        "invalid_pct": 1,
        "hold_s": 14,
        "fallback_s": 0,
        "restarts": 0,
        "fallback_now": 0,
        "risk_pct": 20,
        "flags": flags,
        "priority": priority,
    }


WARD = [facts(1, 0, 8), facts(2, 1 | 4, 500, 71), facts(3, 8, 90, 80)]
WINDOW = model_eval.Window("w", "watch", WARD)
GOOD = json.dumps(
    {
        "notes": [
            {
                "cot": 2,
                "action": "review respiratory support",
                "why": "Rising need and frequent dips mean the nurse should review respiratory support.",
            },
            {"cot": 3, "action": "check the probe site", "why": "The probe signal was lost often."},
            {"cot": 1, "action": "nothing flagged", "why": "Time in target 90 percent."},
        ]
    },
    separators=(",", ":"),
)
WORDS = json.loads((ROOT / "eval" / "model_cases.json").read_text(encoding="utf-8"))["flag_words"]


def test_a_good_answer_passes_the_c_validator() -> None:
    assert model_eval.validate(GOOD, WARD, "watch") == "ok"


@pytest.mark.parametrize(
    "bad, verdict",
    [
        ("not json", "schema"),
        (GOOD.replace("Rising need", "Rising 0.99 need"), "number"),
        (GOOD.replace("Rising need", "Increase oxygen and rising need"), "forbidden"),
        (GOOD.replace("Rising need", "Stable need"), "word"),
        (GOOD.replace("check the probe site", "set FiO2 to 1.00"), "action"),
        (GOOD.replace('"cot":3', '"cot":9'), "cots"),
    ],
)
def test_a_bad_answer_is_refused_for_its_reason(bad: str, verdict: str) -> None:
    assert model_eval.validate(bad, WARD, "watch") == verdict


def test_a_healthy_cot_first_is_refused() -> None:
    swapped = json.dumps(
        {
            "notes": [
                {"cot": 1, "action": "nothing flagged", "why": "Time in target 90 percent."},
                {"cot": 2, "action": "review respiratory support", "why": "Dips."},
                {"cot": 3, "action": "check the probe site", "why": "Probe."},
            ]
        },
        separators=(",", ":"),
    )
    assert model_eval.validate(swapped, WARD, "watch") == "order"


def test_the_handover_uses_its_own_validator() -> None:
    text = json.dumps({"cots": [{"cot": 1, "summary": "Probe trouble."}]}, separators=(",", ":"))
    assert model_eval.validate(text, [facts(1, 8)], "handover") == "ok"
    assert model_eval.validate(text.replace("Probe trouble", "Probe trouble 90"), [facts(1, 8)], "handover") == "number"


def test_a_ward_with_no_flag_is_not_asked_and_not_counted(tmp_path: Path) -> None:
    quiet = model_eval.Window("q", "watch", [facts(1), facts(2)])
    answers = tmp_path / "a.jsonl"
    rows = [
        {"id": "q", "text": "", "validator": model_eval.NOT_ASKED, "generated": 0, "total_ms": 0.0},
        {"id": "w", "text": GOOD, "generated": 60, "gen_ms": 6000.0, "total_ms": 8000.0, "deadline_met": 1},
    ]
    answers.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")
    s = model_eval.score_file(answers, {"q": quiet, "w": WINDOW})
    assert s["answers"] == 1
    assert s["not_asked"] == 1
    assert s["valid_rate"] == 1.0
    assert s["mean_total_s"] == 8.0


def test_an_empty_answer_with_a_flag_on_the_ward_is_a_refusal() -> None:
    s = model_eval.score_answer({"id": "w", "text": ""}, WINDOW, WORDS)
    assert s["validator"] == "schema"


def test_flag_coverage_counts_flags_the_sentence_speaks_to() -> None:
    s = model_eval.score_answer({"id": "w", "text": GOOD}, WINDOW, WORDS)
    assert s["validator"] == "ok"
    assert s["flags_raised"] == 3
    assert s["flags_covered"] == 3


def test_a_sentence_silent_on_a_flag_loses_coverage() -> None:
    silent = GOOD.replace(
        "Rising need and frequent dips mean the nurse should review respiratory support.",
        "The nurse should review support.",
    )
    s = model_eval.score_answer({"id": "w", "text": silent}, WINDOW, WORDS)
    assert s["flags_raised"] == 3
    assert s["flags_covered"] == 1


def test_an_invalid_answer_scores_no_coverage() -> None:
    s = model_eval.score_answer({"id": "w", "text": "garbage"}, WINDOW, WORDS)
    assert s["validator"] == "schema"
    assert "order_tau" not in s


def test_order_agreement_is_kendalls_tau() -> None:
    assert model_eval.order_agreement([2, 3, 1], WARD) == 1.0
    assert model_eval.order_agreement([1, 3, 2], WARD) == -1.0
    assert model_eval.order_agreement([2, 1, 3], WARD) == pytest.approx(1.0 / 3.0)
    assert model_eval.order_agreement([1], [facts(1)]) == 1.0


def test_equal_priorities_count_as_agreement() -> None:
    tied = [facts(1, 0, 5), facts(2, 0, 5)]
    assert model_eval.order_agreement([2, 1], tied) == 1.0


def test_the_first_cot_is_checked_against_the_top_priority() -> None:
    s = model_eval.score_answer({"id": "w", "text": GOOD}, WINDOW, WORDS)
    assert s["first_is_top_priority"] is True
    assert s["order_tau"] == 1.0


def test_windows_round_trip_through_the_format_the_board_reads(tmp_path: Path) -> None:
    text = model_eval.windows_text([WINDOW, model_eval.Window("h", "handover", [facts(1)])])
    path = tmp_path / "w.txt"
    path.write_text(text, encoding="utf-8")
    back = model_eval.read_windows(path)
    assert set(back) == {"w", "h"}
    assert back["w"].kind == "watch"
    assert back["w"].facts == WARD
    assert back["h"].kind == "handover"


def test_a_whole_file_is_summed_up(tmp_path: Path) -> None:
    answers = tmp_path / "a.jsonl"
    rows = [
        {"id": "w", "text": GOOD, "generated": 60, "gen_ms": 6000.0, "total_ms": 8000.0, "deadline_met": 1},
        {"id": "w", "text": "garbage", "generated": 3, "gen_ms": 300.0, "total_ms": 1000.0, "deadline_met": 1},
    ]
    answers.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")
    s = model_eval.score_file(answers, {"w": WINDOW})
    assert s["answers"] == 2
    assert s["valid"] == 1
    assert s["valid_rate"] == 0.5
    assert s["verdicts"] == {"ok": 1, "schema": 1}
    assert s["flag_coverage"] == 1.0
    assert s["gen_tok_s"] == 10.0
    assert s["mean_total_s"] == 4.5
    assert s["deadlines_met"] == 2


def test_answers_without_timings_report_no_timings(tmp_path: Path) -> None:
    answers = tmp_path / "a.jsonl"
    answers.write_text(json.dumps({"id": "w", "text": GOOD, "generated": 60}) + "\n", encoding="utf-8")
    s = model_eval.score_file(answers, {"w": WINDOW})
    assert s["valid"] == 1
    assert "gen_tok_s" not in s
    assert "mean_total_s" not in s
    assert "deadlines_met" not in s


def test_the_case_set_keeps_its_seeds_apart() -> None:
    cfg = json.loads(model_eval.CASES.read_text(encoding="utf-8"))
    assert not set(cfg["seeds"]["development"]) & set(cfg["seeds"]["held_out"])
    assert ["stable", "stable", "stable"] in cfg["wards"]
    assert set(WORDS) == set(model_eval.FLAG_NAMES)


@pytest.mark.integration
def test_a_small_window_file_is_written_from_the_simulator(tmp_path: Path) -> None:
    cfg = json.loads(model_eval.CASES.read_text(encoding="utf-8"))
    cfg["wards"] = [["stable", "moderate", "unstable"]]
    cfg["handover_wards"] = [["stable", "moderate", "unstable"]]
    cfg["end_s"] = [10800]
    cfg["seeds"] = {"development": [0], "held_out": [1000]}
    path = model_eval.write_windows("development", tmp_path, cfg)
    windows = model_eval.read_windows(path)
    assert set(windows) == {"w0-s0-e10800", "h0-s0"}
    assert [f["cot"] for f in windows["w0-s0-e10800"].facts] == [1, 2, 3]
    assert windows["h0-s0"].kind == "handover"


@pytest.mark.parametrize("path", sorted(RESULTS.glob("model_outputs_*.jsonl")), ids=lambda p: p.stem)
def test_recorded_answers_still_pass_the_validator_they_passed_on_the_board(path: Path) -> None:
    set_name = "held_out" if "held_out" in path.stem else "development"
    windows = model_eval.read_windows(ROOT / "eval" / f"model_windows_{set_name}.txt")
    for line in path.read_text(encoding="utf-8").splitlines():
        row = json.loads(line)
        window = windows[row["id"]]
        if row["validator"] == model_eval.NOT_ASKED:
            assert not any(f["flags"] for f in window.facts), row["id"]
        else:
            assert model_eval.validate(row["text"], window.facts, window.kind) == row["validator"], row["id"]
