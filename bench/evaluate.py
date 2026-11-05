"""Closed-loop evaluation on a seed set, and the step tests, written to eval/results/.

    python -m bench evaluate development
    python -m bench evaluate held_out

Every controller runs on the same babies and oximeters, so the comparison is paired run by run.
"""

from __future__ import annotations

import json
import os
import statistics
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from typing import Any

from bench.scenario import draw, families, load, seeds
from bench.simrun import CONTROLLERS, run
from bench.steptest import run_all

ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "eval" / "results"
TUNED = ROOT / "eval" / "tuned_params.json"

HEADLINE = (
    "time_in_target_pct",
    "below_85_pct",
    "below_80_pct",
    "above_98_on_o2_pct",
    "true_time_in_target_pct",
    "adjustments_per_min",
    "max_rise_60s",
    "fallback_s",
)


def tuned_params() -> dict[str, float]:
    if not TUNED.exists():
        return {}
    data: dict[str, float] = json.loads(TUNED.read_text(encoding="utf-8"))
    return data


def _one(job: tuple[str, int, str, dict[str, float], dict[str, Any]]) -> dict[str, Any]:
    family, seed, controller, params, cfg = job
    r = run(draw(family, seed, cfg), controller, params if controller == "titra" else None)
    return {"family": family, "seed": seed, "controller": controller, **r.summary.as_dict()}


def evaluate(
    set_name: str, workers: int = 4, params: dict[str, float] | None = None, cfg: dict[str, Any] | None = None
) -> dict[str, Any]:
    cfg = load() if cfg is None else cfg
    params = tuned_params() if params is None else params
    jobs = [(f, s, c, params, cfg) for f in families(cfg) for s in seeds(set_name, cfg) for c in CONTROLLERS]
    with ProcessPoolExecutor(max_workers=workers) as pool:
        runs = list(pool.map(_one, jobs))
    table: dict[str, dict[str, dict[str, float]]] = {}
    for c in CONTROLLERS:
        table[c] = {}
        for f in [*families(cfg), "all"]:
            rows = [r for r in runs if r["controller"] == c and (f == "all" or r["family"] == f)]
            table[c][f] = {k: statistics.fmean(r[k] for r in rows) for k in HEADLINE}
    paired = [
        a["time_in_target_pct"] - b["time_in_target_pct"]
        for a in runs
        if a["controller"] == "titra"
        for b in runs
        if b["controller"] == "fixed" and (b["family"], b["seed"]) == (a["family"], a["seed"])
    ]
    steps = [
        {
            "id": s.id,
            "name": s.name,
            "spo2": s.spo2,
            "commanded_per_min": s.commanded_per_min,
            "circuit_per_min": s.circuit_per_min,
        }
        for s in run_all(params)
    ]
    return {
        "set": set_name,
        "params": params,
        "runs_per_controller": len(runs) // len(CONTROLLERS),
        "mean": table,
        "titra_minus_fixed_time_in_target": {
            "mean": statistics.fmean(paired),
            "runs_better": sum(1 for d in paired if d > 0),
            "runs": len(paired),
        },
        "step_tests": steps,
        "runs": runs,
    }


def write(result: dict[str, Any]) -> Path:
    RESULTS.mkdir(parents=True, exist_ok=True)
    path = RESULTS / f"{result['set']}.json"
    path.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    return path


def main(set_name: str, workers: int | None = None) -> None:
    result = evaluate(set_name, workers or min(4, os.cpu_count() or 1))
    path = write(result)
    m = result["mean"]
    print(f"{set_name}: {result['runs_per_controller']} runs per controller -> {path.relative_to(ROOT)}")
    print(f"{'family':15s} " + " ".join(f"{c:>8s}" for c in CONTROLLERS) + "   (time in target, %)")
    for f in [*families(), "all"]:
        print(f"{f:15s} " + " ".join(f"{m[c][f]['time_in_target_pct']:8.1f}" for c in CONTROLLERS))
    d = result["titra_minus_fixed_time_in_target"]
    print(f"Titra minus fixed FiO2: {d['mean']:+.1f} points, better on {d['runs_better']} of {d['runs']} runs")
    for s in result["step_tests"]:
        print(
            f"{s['id']} {s['name']}: {s['commanded_per_min']:.3f} FiO2/min commanded, {s['circuit_per_min']:.3f} in circuit"
        )
