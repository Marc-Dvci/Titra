"""Tunes the controller gains on the development seeds only, by coordinate descent.

    python -m bench.tune [--rounds N] [--workers N]

Every evaluation is appended to eval/tuning_log.jsonl, and the best parameters go to eval/tuned_params.json. The
held-out seeds are never read here.
"""

from __future__ import annotations

import argparse
import json
import os
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from typing import Any

from bench.scenario import draw, families, load, seeds
from bench.simrun import run

ROOT = Path(__file__).resolve().parents[1]
LOG = ROOT / "eval" / "tuning_log.jsonl"
BEST = ROOT / "eval" / "tuned_params.json"

START: dict[str, float] = {
    "kp_lo": 0.0040,
    "ki_lo": 0.00045,
    "kp_in": 0.0010,
    "ki_in": 0.00005,
    "kp_hi": 0.0020,
    "ki_hi": 0.00020,
    "rate_up": 0.010,
    "rate_down": 0.005,
}
STEP = 1.6
# The rate limits stay below 0.0175 per second, so that one update over the longest interval (2 s) plus rounding
# stays inside the blender driver's slew limit of 0.05.
BOUNDS: dict[str, tuple[float, float]] = {"rate_up": (0.002, 0.0175), "rate_down": (0.001, 0.0175)}


def objective(m: dict[str, float]) -> float:
    """Time in target, with time below 80% and time above 98% on oxygen counted against it."""
    return m["time_in_target_pct"] - 3.0 * m["below_80_pct"] - 1.0 * m["above_98_on_o2_pct"]


def _one(job: tuple[str, int, dict[str, float]]) -> dict[str, float]:
    family, seed, params = job
    return run(draw(family, seed), "titra", params).summary.as_dict()


def evaluate(params: dict[str, float], pool: ProcessPoolExecutor, cfg: dict[str, Any]) -> dict[str, float]:
    jobs = [(f, s, params) for f in families(cfg) for s in seeds("development", cfg)]
    results = list(pool.map(_one, jobs))
    keys = results[0].keys()
    mean = {k: sum(r[k] for r in results) / len(results) for k in keys}
    mean["objective"] = sum(objective(r) for r in results) / len(results)
    return mean


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--workers", type=int, default=min(4, os.cpu_count() or 1))
    a = ap.parse_args()
    cfg = load()
    best = dict(START)
    if BEST.exists():
        best.update(json.loads(BEST.read_text(encoding="utf-8")))
    with ProcessPoolExecutor(max_workers=a.workers) as pool, LOG.open("a", encoding="utf-8") as log:

        def score(p: dict[str, float]) -> float:
            m = evaluate(p, pool, cfg)
            log.write(json.dumps({"params": p, "metrics": m}) + "\n")
            log.flush()
            return m["objective"]

        best_score = score(best)
        print(f"start {best_score:.3f}", flush=True)
        for rnd in range(a.rounds):
            improved = False
            for name in START:
                for factor in (STEP, 1.0 / STEP):
                    trial = dict(best)
                    trial[name] = best[name] * factor
                    lo, hi = BOUNDS.get(name, (0.0, float("inf")))
                    if not lo <= trial[name] <= hi:
                        continue
                    s = score(trial)
                    if s > best_score + 0.05:
                        best, best_score, improved = trial, s, True
                        print(f"round {rnd} {name} x{factor:.3f} -> {s:.3f}", flush=True)
                        BEST.write_text(json.dumps(best, indent=2) + "\n", encoding="utf-8")
                        break
            if not improved:
                break
    print(f"best {best_score:.3f} {best}", flush=True)


if __name__ == "__main__":
    main()
