"""Scenario families and seeds from eval/scenarios.json, drawn into concrete patients and oximeters."""

from __future__ import annotations

import json
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from bench.oximeter import OximeterParams
from bench.patient import PatientParams, fio2_for_saturation

ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = ROOT / "eval" / "scenarios.json"


def load(path: Path = SCENARIOS) -> dict[str, Any]:
    data: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    return data


@dataclass(frozen=True)
class CotSettings:
    fio2_min: float
    fio2_max: float
    backup_fio2: float


@dataclass(frozen=True)
class Scenario:
    family: str
    seed: int
    duration_s: int
    patient: PatientParams
    oximeter: OximeterParams
    settings: CotSettings
    initial_fio2: float

    def rng(self, stream: str) -> random.Random:
        """An independent random stream per component, so every controller faces the same baby and oximeter."""
        return random.Random(f"{self.family}:{self.seed}:{stream}")


def _pair(v: Any) -> tuple[float, float]:
    lo, hi = v
    return float(lo), float(hi)


def draw(family: str, seed: int, cfg: dict[str, Any] | None = None) -> Scenario:
    cfg = cfg or load()
    fam = cfg["families"][family]
    common = cfg["common"]
    r = random.Random(f"{family}:{seed}:draw")

    def u(name: str, src: dict[str, Any] = fam) -> float:
        lo, hi = _pair(src[name])
        return r.uniform(lo, hi)

    duration = int(cfg["duration_s"])
    patient = PatientParams(
        shunt=u("shunt"),
        vq_deficit_mmHg=u("vq_deficit_mmHg"),
        paco2_mmHg=u("paco2_mmHg", common),
        apnoea_per_h=u("apnoea_per_h"),
        apnoea_duration_s=_pair(common["apnoea_duration_s"]),
        shunt_episodes_per_h=u("shunt_episodes_per_h"),
        shunt_episode_size=_pair(common["shunt_episode_size"]),
        shunt_episode_minutes=_pair(common["shunt_episode_minutes"]),
        shunt_ramp=float(fam.get("shunt_ramp", 0.0)),
        vq_deficit_ramp_mmHg=float(fam.get("vq_deficit_ramp_mmHg", 0.0)),
        duration_s=float(duration),
    )
    oximeter = OximeterParams(
        artefact_flagged_per_h=u("artefact_flagged_per_h"),
        artefact_unflagged_per_h=u("artefact_unflagged_per_h"),
        sensor_off_per_h=u("sensor_off_per_h"),
        sensor_off_duration_s=_pair(common["sensor_off_duration_s"]),
    )
    s = cfg["cot_settings"]
    settings = CotSettings(float(s["fio2_min"]), float(s["fio2_max"]), float(s["backup_fio2"]))
    need = fio2_for_saturation(0.93, patient.shunt, patient.vq_deficit_mmHg, patient.paco2_mmHg)
    initial = min(max(round(need, 2), settings.fio2_min), settings.fio2_max)
    return Scenario(family, seed, duration, patient, oximeter, settings, initial)


def seeds(set_name: str, cfg: dict[str, Any] | None = None) -> list[int]:
    cfg = cfg or load()
    return [int(s) for s in cfg["seeds"][set_name]]


def families(cfg: dict[str, Any] | None = None) -> list[str]:
    cfg = cfg or load()
    return list(cfg["families"])
