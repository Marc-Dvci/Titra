"""Command line for the bench twin.

python -m bench sim FAMILY SEED [--controller titra|fixed|manual] [--csv FILE]
python -m bench steps
python -m bench evaluate development|held_out [--workers N]
python -m bench flags development|held_out
python -m bench risk development|held_out
python -m bench model-cases development|held_out
python -m bench model-score ANSWERS.jsonl WINDOWS.txt
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

from bench import evaluate, flags, model_eval, risk
from bench.scenario import draw, families
from bench.simrun import CONTROLLERS, run
from bench.steptest import run_all


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m bench")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sim", help="one closed-loop run on a simulated baby")
    s.add_argument("family", choices=families())
    s.add_argument("seed", type=int)
    s.add_argument("--controller", choices=CONTROLLERS, default="titra")
    s.add_argument("--csv", type=Path)
    sub.add_parser("steps", help="the open-loop step tests")
    e = sub.add_parser("evaluate", help="every family, seed and controller of a seed set")
    e.add_argument("set", choices=("development", "held_out"))
    e.add_argument("--workers", type=int)
    g = sub.add_parser("flags", help="score the advisor's flags on a seed set")
    g.add_argument("set", choices=("development", "held_out"))
    rk = sub.add_parser("risk", help="score the early-warning model and its baselines on a seed set")
    rk.add_argument("set", choices=("development", "held_out"))
    mc = sub.add_parser("model-cases", help="write the windows the board replays through the model")
    mc.add_argument("set", choices=("development", "held_out"))
    ms = sub.add_parser("model-score", help="score recorded model answers against their windows")
    ms.add_argument("answers", type=Path)
    ms.add_argument("windows", type=Path)
    a = ap.parse_args(argv)

    if a.cmd == "sim":
        params = evaluate.tuned_params() if a.controller == "titra" else None
        r = run(draw(a.family, a.seed), a.controller, params)
        for k, v in r.summary.as_dict().items():
            print(f"{k:26s} {v:10.3f}")
        if a.csv:
            with a.csv.open("w", newline="", encoding="utf-8") as fh:
                w = csv.writer(fh)
                w.writerow(["t", "sa_true", "spo2", "flags", "fio2", "circuit", "mode", "alarm"])
                for x in r.trace:
                    w.writerow(
                        [
                            x.t,
                            f"{x.sa_true:.4f}",
                            "" if x.spo2 is None else x.spo2,
                            x.flags,
                            f"{x.fio2:.3f}",
                            f"{x.circuit:.4f}",
                            x.mode,
                            x.alarm,
                        ]
                    )
    elif a.cmd == "steps":
        for st in run_all(evaluate.tuned_params()):
            print(
                f"{st.id} {st.name} (SpO2 {st.spo2}%): {st.commanded_per_min:.3f} FiO2/min commanded, "
                f"{st.circuit_per_min:.3f} in the circuit"
            )
    elif a.cmd == "flags":
        result = flags.write_results(a.set)
        print(json.dumps(result["summary"], indent=1))
    elif a.cmd == "risk":
        print(json.dumps(risk.write_results(a.set), indent=1))
    elif a.cmd == "model-cases":
        print(model_eval.write_windows(a.set))
    elif a.cmd == "model-score":
        print(json.dumps(model_eval.score_file(a.answers, model_eval.read_windows(a.windows)), indent=1))
    else:
        evaluate.main(a.set, a.workers)
    return 0


if __name__ == "__main__":
    sys.exit(main())
