"""Data and scoring for the early-warning model (core/src/risk.c, eval/risk_cases.json).

    python -m bench risk development     the chosen model and the baselines on the development seeds
    python -m bench risk held_out        once, after the choice is final

The features come from the C core through ctypes, so training and the board compute the same numbers. Ground truth is the
simulator's true saturation, which the model never sees.
"""

from __future__ import annotations

import ctypes
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from bench.core import lib as core_lib
from bench.flags import SecondC, to_seconds
from bench.metrics import Second as TraceSecond
from bench.scenario import draw
from bench.simrun import run

ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT / "eval" / "risk_cases.json"
N_FEATURES = 13
NAMES = (
    "spo2_mean_15",
    "spo2_sd_15",
    "spo2_min_15",
    "below_91_pct_15",
    "below_85_pct_15",
    "dips_15",
    "dips_60",
    "fio2_mean_15",
    "fio2_change_15",
    "fio2_sd_15",
    "above_95_pct_15",
    "invalid_pct_15",
    "seconds_since_dip",
)


def _lib() -> ctypes.CDLL:
    lib = core_lib()
    lib.tr_score.restype = ctypes.c_double
    lib.tr_score.argtypes = [ctypes.POINTER(ctypes.c_double)]
    lib.tr_features.argtypes = [
        ctypes.POINTER(SecondC),
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_double),
    ]
    return lib


def features(seconds: ctypes.Array[SecondC], end_s: int) -> list[float]:
    """The features at end_s, from the hour before it, by the C core."""
    n = min(end_s, 3600)
    out = (ctypes.c_double * N_FEATURES)()
    _lib().tr_features(seconds, len(seconds), end_s - n, n, out)
    return list(out)


def score(row: list[float]) -> float:
    """The trained model's probability, by the C core."""
    arr = (ctypes.c_double * N_FEATURES)(*row)
    return float(_lib().tr_score(arr))


def dip_starts(trace: list[TraceSecond], below: float, min_s: int) -> list[int]:
    """When each true dip begins: saturation below `below` for at least min_s seconds."""
    starts = []
    begin = -1
    for i, s in enumerate(trace):
        if s.sa_true < below:
            if begin < 0:
                begin = i
            if i - begin + 1 == min_s:
                starts.append(begin)
        else:
            begin = -1
    return starts


def in_dip(trace: list[TraceSecond], t: int, below: float) -> bool:
    return trace[t].sa_true < below


@dataclass(frozen=True)
class Row:
    family: str
    seed: int
    t: int
    x: list[float]
    y: int
    next_dip_in_s: int  # seconds until the next dip starts, or -1


def build(set_name: str, cfg: dict[str, Any] | None = None) -> list[Row]:
    cfg = cfg or json.loads(CASES.read_text(encoding="utf-8"))
    rows: list[Row] = []
    below = float(cfg["below"])
    min_s = int(cfg["min_s"])
    horizon = int(cfg["horizon_s"])
    for family in cfg["families"]:
        for seed in cfg["seeds"][set_name]:
            trace = run(draw(family, int(seed)), "titra").trace
            seconds = to_seconds(trace)
            starts = dip_starts(trace, below, min_s)
            last = len(trace) - horizon
            for t in range(int(cfg["first_s"]), last, int(cfg["step_s"])):
                if in_dip(trace, t, below):
                    continue
                upcoming = next((s for s in starts if s > t), -1)
                y = int(upcoming > 0 and upcoming - t <= horizon)
                rows.append(Row(family, int(seed), t, features(seconds, t), y, upcoming - t if upcoming > 0 else -1))
    return rows


def auroc(scores: list[float], labels: list[int]) -> float:
    """Area under the ROC curve, with ties shared."""
    order = sorted(range(len(scores)), key=lambda i: scores[i])
    ranks = [0.0] * len(scores)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and scores[order[j + 1]] == scores[order[i]]:
            j += 1
        for k in range(i, j + 1):
            ranks[order[k]] = (i + j) / 2.0 + 1.0
        i = j + 1
    pos = sum(labels)
    neg = len(labels) - pos
    if pos == 0 or neg == 0:
        return 0.5
    total = sum(r for r, y in zip(ranks, labels, strict=True) if y)
    return (total - pos * (pos + 1) / 2.0) / (pos * neg)


def caught_at_alert_rate(rows: list[Row], scores: list[float], alerts_per_hour: float, step_s: int) -> dict[str, float]:
    """The share of dip-bearing minutes flagged when the threshold allows this many alert minutes per cot per hour."""
    n_alerts = int(len(rows) * (alerts_per_hour * step_s / 3600.0))
    ranked = sorted(range(len(rows)), key=lambda i: scores[i], reverse=True)[:n_alerts]
    hits = sum(rows[i].y for i in ranked)
    pos = sum(r.y for r in rows)
    return {
        "alert_minutes": n_alerts,
        "true_alerts": hits,
        "precision": round(hits / max(n_alerts, 1), 3),
        "recall": round(hits / max(pos, 1), 3),
    }


def baseline_scores(rows: list[Row], name: str) -> list[float]:
    index = {"dips_85_last_hour": 6, "time_below_91_last_15_min": 3, "spo2_sd_last_15_min": 1}[name]
    return [r.x[index] for r in rows]


def evaluate(set_name: str) -> dict[str, Any]:
    cfg = json.loads(CASES.read_text(encoding="utf-8"))
    rows = build(set_name, cfg)
    labels = [r.y for r in rows]
    step = int(cfg["step_s"])
    rate = float(cfg["alerts_per_hour"])
    out: dict[str, Any] = {
        "set": set_name,
        "minutes": len(rows),
        "minutes_with_a_dip_ahead": sum(labels),
        "base_rate": round(sum(labels) / max(len(labels), 1), 4),
        "alerts_per_hour": rate,
        "scorers": {},
    }
    scorers = {name: baseline_scores(rows, name) for name in cfg["baselines"]}
    scorers["model"] = [score(r.x) for r in rows]
    for name, values in scorers.items():
        out["scorers"][name] = {
            "auroc": round(auroc(values, labels), 4),
            "at_alert_rate": caught_at_alert_rate(rows, values, rate, step),
        }
    return out


def write_results(set_name: str, out_dir: Path = ROOT / "eval" / "results") -> dict[str, Any]:
    result = evaluate(set_name)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / f"risk_{set_name}.json").write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    return result
