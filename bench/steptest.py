"""Open-loop step tests after Salverda et al. (2026): the oximeter reports a fixed SpO2 whatever the FiO2."""

from __future__ import annotations

import json
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from bench.blender import Blender
from bench.core import CotChain
from bench.frames import parse_bl
from bench.metrics import max_change_in_window
from bench.oximeter import Oximeter, OximeterParams

ROOT = Path(__file__).resolve().parents[1]
STEP_TESTS = ROOT / "eval" / "step_tests.json"


@dataclass(frozen=True)
class StepResult:
    id: str
    name: str
    spo2: int
    commanded_per_min: float
    circuit_per_min: float
    commanded: list[float]
    circuit: list[float]


def run_step(test: dict[str, Any], cfg: dict[str, Any], params: dict[str, float] | None = None) -> StepResult:
    s = cfg["cot_settings"]
    window = int(cfg["window_s"])
    start = float(test["start_fio2"])
    chain = CotChain(float(s["fio2_min"]), float(s["fio2_max"]), float(s["backup_fio2"]), start, 0.0, params)
    # No artefact or sensor-off episodes: the step test isolates the controller's response.
    ox = Oximeter(random.Random(f"step:{test['id']}"), OximeterParams(0.0, 0.0, 0.0))
    blender = Blender(backup_fio2=float(s["backup_fio2"]), fio2=start)
    commanded: list[float] = []
    circuit: list[float] = []
    settle, hold = int(test["settle_s"]), int(test["hold_s"])
    for k in range(settle + hold):
        t = float(k)
        ox.fixed_spo2 = int(test["settle_spo2"]) if k < settle else int(test["spo2"])
        chain.feed(ox.transmit(0.93, 155.0), t + 0.05)
        bl, _ = chain.tick(t + 0.5)
        frame = parse_bl(bl.decode("ascii").rstrip("\r\n"))
        blender.on_frame(t + 0.5, frame)
        c = blender.step(t + 1.0)
        if k >= settle:
            commanded.append(frame.fio2_pm / 1000.0)
            circuit.append(c)
    direction = +1 if int(test["spo2"]) < int(test["settle_spo2"]) else -1
    per_min = 60.0 / window
    return StepResult(
        str(test["id"]),
        str(test["name"]),
        int(test["spo2"]),
        max_change_in_window(commanded, window, direction) * per_min,
        max_change_in_window(circuit, window, direction) * per_min,
        commanded,
        circuit,
    )


def run_all(params: dict[str, float] | None = None, path: Path = STEP_TESTS) -> list[StepResult]:
    cfg: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    return [run_step(t, cfg, params) for t in cfg["tests"]]
