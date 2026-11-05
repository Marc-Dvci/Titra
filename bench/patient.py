"""The simulated preterm baby of docs/MODEL.md: oxygen uptake, lung disease, apnoea and shunt episodes."""

from __future__ import annotations

import math
import random
from collections import deque
from dataclasses import dataclass

PB_MMHG = 745.0  # barometric pressure near 230 m altitude
PH2O_MMHG = 47.0
RQ = 0.8
P50_NEONATAL = 20.0
P50_ADULT = 26.8
AV_DIFF_ML_DL = 3.0  # oxygen consumption 7 mL/kg/min over a cardiac output near 230 mL/kg/min
HB_G_DL = 15.0
HUFNER = 1.34  # mL O2 per g of haemoglobin
DISSOLVED = 0.003  # mL O2 per dL of blood per mmHg
LUNG_TAU_S = 15.0
APNOEA_DECAY_PER_S = 0.007  # alveolar oxygen fraction during apnoea, see docs/MODEL.md
CIRCULATION_DELAY_S = 5
SHUNT_OU_TAU_S = 1800.0
SHUNT_OU_SD = 0.02


def severinghaus(p_mmhg: float) -> float:
    """Saturation (0 to 1) for an adult-equivalent oxygen tension (Severinghaus 1979)."""
    if p_mmhg <= 0.0:
        return 0.0
    return 1.0 / (23400.0 / (p_mmhg**3 + 150.0 * p_mmhg) + 1.0)


def neonatal_saturation(p_mmhg: float) -> float:
    """Saturation for a neonatal P50 of 20 mmHg, by scaling the tension onto the adult curve."""
    return severinghaus(p_mmhg * P50_ADULT / P50_NEONATAL)


def lag(x: float, target: float, tau_s: float, dt: float) -> float:
    """Exact discrete first-order lag."""
    return x + (target - x) * (1.0 - math.exp(-dt / tau_s))


@dataclass(frozen=True)
class PatientParams:
    shunt: float
    vq_deficit_mmHg: float
    paco2_mmHg: float
    apnoea_per_h: float
    apnoea_duration_s: tuple[float, float]
    shunt_episodes_per_h: float
    shunt_episode_size: tuple[float, float]
    shunt_episode_minutes: tuple[float, float]
    shunt_ramp: float = 0.0
    vq_deficit_ramp_mmHg: float = 0.0
    duration_s: float = 14400.0


def content(p_mmhg: float) -> float:
    """Oxygen content (mL/dL) of blood at tension p: bound to haemoglobin plus dissolved."""
    return HUFNER * HB_G_DL * neonatal_saturation(p_mmhg) + DISSOLVED * max(p_mmhg, 0.0)


def saturation_for_content(c: float) -> float:
    """Saturation of blood with oxygen content c, by bisection on the tension."""
    if c <= 0.0:
        return 0.0
    lo, hi = 0.0, 760.0
    if content(hi) <= c:
        return neonatal_saturation(hi)
    for _ in range(50):
        mid = (lo + hi) / 2.0
        if content(mid) < c:
            lo = mid
        else:
            hi = mid
    return neonatal_saturation(hi)


def arterial_saturation(fa: float, shunt: float, vq_deficit: float, paco2: float) -> float:
    """Steady-state arterial saturation (0 to 1) for an alveolar oxygen fraction `fa`.

    End-capillary blood leaves the alveoli at PAO2 minus the ventilation-perfusion deficit. A fraction `shunt` of
    the cardiac output bypasses ventilated alveoli and carries mixed venous blood, whose content is the arterial
    content minus the arteriovenous difference. Solving the mixing equation for the arterial content gives
    Ca = Cc - shunt / (1 - shunt) * D.
    """
    pao2 = max(fa * (PB_MMHG - PH2O_MMHG) - paco2 / RQ, 0.0)
    cc = content(max(pao2 - vq_deficit, 0.0))
    f = min(max(shunt, 0.0), 0.95)
    return saturation_for_content(cc - f / (1.0 - f) * AV_DIFF_ML_DL)


def fio2_for_saturation(target: float, shunt: float, vq_deficit: float, paco2: float) -> float:
    """The steady-state FiO2 that gives `target` saturation, by bisection on [0.21, 1.0]."""
    lo, hi = 0.21, 1.0
    if arterial_saturation(lo, shunt, vq_deficit, paco2) >= target:
        return lo
    if arterial_saturation(hi, shunt, vq_deficit, paco2) < target:
        return hi
    for _ in range(60):
        mid = (lo + hi) / 2.0
        if arterial_saturation(mid, shunt, vq_deficit, paco2) < target:
            lo = mid
        else:
            hi = mid
    return hi


class Patient:
    """Advances in 1 s steps. `step` takes the circuit FiO2 and returns the saturation at the oximeter site."""

    def __init__(self, params: PatientParams, rng: random.Random, initial_fio2: float) -> None:
        self.p = params
        self.rng = rng
        self.t = 0.0
        self.fa = initial_fio2
        self.shunt_ou = params.shunt
        self.apnoea_left = 0.0
        self.apnoea_len = 0.0
        self.episodes: list[tuple[float, float, float]] = []  # (start, length, size)
        sa = arterial_saturation(initial_fio2, params.shunt, params.vq_deficit_mmHg, params.paco2_mmHg)
        self.sa_line: deque[float] = deque([sa] * CIRCULATION_DELAY_S, maxlen=CIRCULATION_DELAY_S)
        self.sa = sa
        self.pr_base = rng.uniform(145.0, 165.0)
        self.pr = self.pr_base

    def mean_shunt(self) -> float:
        return self.p.shunt + self.p.shunt_ramp * min(self.t / self.p.duration_s, 1.0)

    def vq_deficit(self) -> float:
        return max(self.p.vq_deficit_mmHg + self.p.vq_deficit_ramp_mmHg * min(self.t / self.p.duration_s, 1.0), 0.0)

    def episode_shunt(self) -> float:
        extra = 0.0
        for start, length, size in self.episodes:
            u = self.t - start
            if u < 0.0 or u > length + 120.0:
                continue
            ramp_in = min(u / 60.0, 1.0)
            ramp_out = 1.0 if u <= length else max(1.0 - (u - length) / 120.0, 0.0)
            extra += size * ramp_in * ramp_out
        return extra

    def shunt(self) -> float:
        return min(max(self.shunt_ou + self.episode_shunt(), 0.0), 0.6)

    @property
    def in_apnoea(self) -> bool:
        return self.apnoea_left > 0.0

    def _events(self, dt: float) -> None:
        p = self.p
        if not self.in_apnoea and self.rng.random() < p.apnoea_per_h * dt / 3600.0:
            self.apnoea_len = self.rng.uniform(*p.apnoea_duration_s)
            self.apnoea_left = self.apnoea_len
        if self.rng.random() < p.shunt_episodes_per_h * dt / 3600.0:
            minutes = self.rng.uniform(*p.shunt_episode_minutes)
            self.episodes.append((self.t, minutes * 60.0, self.rng.uniform(*p.shunt_episode_size)))
        self.episodes = [e for e in self.episodes if self.t - e[0] <= e[1] + 120.0]
        mean = self.mean_shunt()
        sigma = SHUNT_OU_SD * math.sqrt(2.0 / SHUNT_OU_TAU_S)
        self.shunt_ou += (mean - self.shunt_ou) * dt / SHUNT_OU_TAU_S + sigma * math.sqrt(dt) * self.rng.gauss(0, 1)

    def step(self, circuit_fio2: float, dt: float = 1.0) -> float:
        self._events(dt)
        if self.in_apnoea:
            self.fa *= math.exp(-APNOEA_DECAY_PER_S * dt)
            self.apnoea_left -= dt
            depth = min(self.apnoea_len / 25.0, 1.0)
            self.pr = lag(self.pr, self.pr_base - 60.0 * depth, 4.0, dt)
        else:
            self.fa = lag(self.fa, circuit_fio2, LUNG_TAU_S, dt)
            self.pr = lag(self.pr, self.pr_base, 8.0, dt)
        sa_now = arterial_saturation(self.fa, self.shunt(), self.vq_deficit(), self.p.paco2_mmHg)
        self.sa = self.sa_line[0]
        self.sa_line.append(sa_now)
        self.t += dt
        return self.sa
