"""Scores the advisor's flags (core/src/facts.c) on the simulator's scenario families (eval/flag_cases.json).

    python -m bench flags development
    python -m bench flags held_out

The windows are cut from four-hour closed-loop runs with Titra's own controller, so the flags see what they would see
on the board. Ground truth comes from how each family is built (`worsening` ramps its need up, `improving` ramps it
down, `artefact_heavy` loses its signal often, `stable` must raise nothing) and, for desaturations, from the simulator's
true saturation, which the flag never sees.
"""

from __future__ import annotations

import ctypes
import json
from pathlib import Path
from typing import Any

from bench.core import lib as core_lib
from bench.metrics import Second as TraceSecond
from bench.scenario import draw
from bench.simrun import run

ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT / "eval" / "flag_cases.json"
FLAGS = {
    1: "rising_need",
    2: "wean_opportunity",
    4: "frequent_desaturations",
    8: "probe_trouble",
    16: "hold_or_fallback",
    32: "high_need",
    64: "system_restart",
}
SAMPLE_VALID, SAMPLE_INVALID, NO_SAMPLE = 0, 1, 255


class SecondC(ctypes.Structure):
    _fields_ = [
        ("spo2", ctypes.c_int16),
        ("fio2_pm", ctypes.c_int16),
        ("mode", ctypes.c_uint8),
        ("alarm", ctypes.c_uint8),
        ("status", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8),
    ]


class FactsC(ctypes.Structure):
    _fields_ = [
        ("cot", ctypes.c_int),
        ("seconds", ctypes.c_int),
        ("valid_seconds", ctypes.c_int),
        ("time_in_target_pct", ctypes.c_int),
        ("above_95_on_o2_pct", ctypes.c_int),
        ("below_85_pct", ctypes.c_int),
        ("desat_episodes", ctypes.c_int),
        ("longest_desat_s", ctypes.c_int),
        ("fio2_early_pm", ctypes.c_int),
        ("fio2_late_pm", ctypes.c_int),
        ("fio2_max_pm", ctypes.c_int),
        ("probe_events", ctypes.c_int),
        ("invalid_pct", ctypes.c_int),
        ("hold_s", ctypes.c_int),
        ("fallback_s", ctypes.c_int),
        ("restarts", ctypes.c_int),
        ("fallback_now", ctypes.c_int),
        ("risk_pct", ctypes.c_int),
        ("flags", ctypes.c_uint32),
        ("priority", ctypes.c_int),
    ]


class ThresholdsC(ctypes.Structure):
    _fields_ = [
        ("min_trend_window_s", ctypes.c_int),
        ("min_rate_window_s", ctypes.c_int),
        ("rising_delta", ctypes.c_double),
        ("wean_delta", ctypes.c_double),
        ("desats_per_h", ctypes.c_double),
        ("probe_per_h", ctypes.c_double),
        ("hold_s_per_h", ctypes.c_double),
        ("fallback_s_per_h", ctypes.c_double),
        ("high_fio2", ctypes.c_double),
        ("high_fio2_pct", ctypes.c_double),
    ]


def _lib() -> ctypes.CDLL:
    lib = core_lib()
    lib.tf_thresholds_default.restype = ThresholdsC
    lib.tf_compute.argtypes = [
        ctypes.POINTER(SecondC),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ThresholdsC),
        ctypes.POINTER(FactsC),
    ]
    lib.tf_second_size.restype = ctypes.c_size_t
    lib.tf_facts_size.restype = ctypes.c_size_t
    if lib.tf_second_size() != ctypes.sizeof(SecondC) or lib.tf_facts_size() != ctypes.sizeof(FactsC):
        raise RuntimeError("the bench's view of the facts structures does not match the C core")
    return lib


def default_thresholds() -> ThresholdsC:
    result: ThresholdsC = _lib().tf_thresholds_default()
    return result


def true_dips_per_h(trace: list[TraceSecond], below: float, min_s: int) -> float:
    """Dips of the simulator's true saturation per hour, the ground truth the flag never sees."""
    count = run_s = 0
    for s in trace:
        if s.sa_true < below:
            run_s += 1
            if run_s == min_s:
                count += 1
        else:
            run_s = 0
    return count / (len(trace) / 3600.0) if trace else 0.0


def to_seconds(trace: list[TraceSecond]) -> ctypes.Array[SecondC]:
    """A trace from the simulator as the samples the advisor reads on the board."""
    out = (SecondC * len(trace))()
    for i, s in enumerate(trace):
        valid = s.spo2 is not None and s.flags == 0
        out[i].spo2 = s.spo2 if s.spo2 is not None else -1
        out[i].fio2_pm = int(round(s.fio2 * 1000))
        out[i].mode = s.mode
        out[i].alarm = s.alarm
        out[i].status = SAMPLE_VALID if valid else SAMPLE_INVALID
    return out


def compute(
    seconds: ctypes.Array[SecondC],
    end_s: int,
    window_s: int,
    cot: int = 1,
    restarts: int = 0,
    thresholds: ThresholdsC | None = None,
) -> FactsC:
    """The facts of the last window_s seconds before end_s, by the C core."""
    th = thresholds if thresholds is not None else default_thresholds()
    start = max(end_s - window_s, 0)
    facts = FactsC()
    _lib().tf_compute(
        seconds,
        len(seconds),
        start,
        end_s - start,
        cot,
        restarts,
        ctypes.byref(th),
        ctypes.byref(facts),
    )
    return facts


def flag_names(bits: int) -> list[str]:
    return [name for bit, name in FLAGS.items() if bits & bit]


def facts_dict(f: FactsC) -> dict[str, int]:
    return {field[0]: int(getattr(f, field[0])) for field in FactsC._fields_}


def score(rows: list[dict[str, Any]], window_s: int) -> dict[str, Any]:
    """Recall of each expected flag, false alarms, and the healthy control."""
    per_flag: dict[str, dict[str, int]] = {}
    per_case: dict[str, dict[str, int]] = {}
    stable_raised = stable_windows = false_alarms = 0
    for r in rows:
        raised = set(r["raised"])
        key = f"{r['family']}@{r['end_s']}"
        c = per_case.setdefault(key, {"windows": 0, "hit": 0, "false_alarm": 0})
        c["windows"] += 1
        for flag in r["expect"]:
            p = per_flag.setdefault(flag, {"expected": 0, "raised": 0})
            p["expected"] += 1
            if flag in raised:
                p["raised"] += 1
                c["hit"] += 1
        extra = raised - set(r["expect"]) - set(r["allow"])
        if extra:
            c["false_alarm"] += 1
            false_alarms += 1
        if r["family"] == "stable":
            stable_windows += 1
            stable_raised += 1 if raised else 0
    return {
        "window_s": window_s,
        "windows": len(rows),
        "stable_windows": stable_windows,
        "stable_windows_with_a_flag": stable_raised,
        "windows_with_a_false_alarm": false_alarms,
        "recall": {k: round(v["raised"] / max(v["expected"], 1), 3) for k, v in sorted(per_flag.items())},
        "expected": {k: v["expected"] for k, v in sorted(per_flag.items())},
        "per_case": per_case,
    }


def evaluate(set_name: str, thresholds: ThresholdsC | None = None, cases_path: Path = CASES) -> dict[str, Any]:
    cfg = json.loads(cases_path.read_text(encoding="utf-8"))
    window_s = int(cfg["window_s"])
    families = sorted({c["family"] for c in cfg["cases"]})
    truth = cfg["desat_truth"]
    traces = {}
    for family in families:
        for seed in cfg["seeds"][set_name]:
            traces[(family, int(seed))] = run(draw(family, int(seed)), "titra").trace
    rows: list[dict[str, Any]] = []
    for case in cfg["cases"]:
        for seed in cfg["seeds"][set_name]:
            trace = traces[(case["family"], int(seed))]
            end_s = int(case["end_s"])
            facts = compute(to_seconds(trace), end_s, window_s, thresholds=thresholds)
            true_rate = true_dips_per_h(
                trace[max(end_s - window_s, 0) : end_s], float(truth["below"]), int(truth["min_s"])
            )
            expect = list(case["expect"])
            allow = list(case["allow"])
            if true_rate >= float(truth["flag_at_per_h"]):
                expect.append("frequent_desaturations")
            elif true_rate >= float(truth["clear_below_per_h"]):
                allow.append("frequent_desaturations")
            rows.append(
                {
                    "family": case["family"],
                    "seed": int(seed),
                    "end_s": end_s,
                    "true_dips_per_h": round(true_rate, 2),
                    "expect": expect,
                    "allow": allow,
                    "raised": flag_names(int(facts.flags)),
                    "facts": facts_dict(facts),
                }
            )
    return {"set": set_name, "summary": score(rows, window_s), "rows": rows}


def write_results(set_name: str, out_dir: Path = ROOT / "eval" / "results", cases_path: Path = CASES) -> dict[str, Any]:
    result = evaluate(set_name, cases_path=cases_path)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / f"flags_{set_name}.json").write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    return result
