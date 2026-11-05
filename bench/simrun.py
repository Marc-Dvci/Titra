"""Closed-loop runs on the simulated preterm, with Titra's C core or a reference controller in the loop."""

from __future__ import annotations

from dataclasses import dataclass

from bench.blender import Blender
from bench.core import CotChain
from bench.frames import BlFrame, LineSplitter, parse_bl, parse_ox
from bench.metrics import Second, Summary, summarise
from bench.oximeter import Oximeter
from bench.patient import Patient
from bench.reference import FixedFio2, ManualCare
from bench.scenario import Scenario

CONTROLLERS = ("titra", "fixed", "manual")
RX_DELAY_S = 0.05  # oximeter frame on the wire
TICK_OFFSET_S = 0.5  # controller tick inside each second


@dataclass
class Run:
    scenario: Scenario
    controller: str
    trace: list[Second]
    summary: Summary
    frames_rejected: int = 0


def displayed(data: bytes, splitter: LineSplitter) -> tuple[int | None, int]:
    """SpO2 and flags as a monitor next to the cot would show them, from the bytes on the line."""
    spo2: int | None = None
    flags = 0
    for line in splitter.feed(data):
        try:
            f = parse_ox(line)
        except ValueError:
            continue
        spo2, flags = f.spo2, f.flags
    return spo2, flags


def run(scn: Scenario, controller: str = "titra", params: dict[str, float] | None = None) -> Run:
    if controller not in CONTROLLERS:
        raise ValueError(f"unknown controller {controller!r}")
    s = scn.settings
    patient = Patient(scn.patient, scn.rng("patient"), scn.initial_fio2)
    ox = Oximeter(scn.rng("oximeter"), scn.oximeter)
    blender = Blender(backup_fio2=s.backup_fio2, fio2=scn.initial_fio2)
    splitter = LineSplitter()
    chain = (
        CotChain(s.fio2_min, s.fio2_max, s.backup_fio2, scn.initial_fio2, 0.0, params)
        if controller == "titra"
        else None
    )
    ref = FixedFio2(scn.initial_fio2) if controller == "fixed" else ManualCare(scn.rng("nurse"), scn.initial_fio2)
    backup_pm = int(round(s.backup_fio2 * 1000))
    circuit = scn.initial_fio2
    trace: list[Second] = []
    mode = alarm = 0
    for k in range(scn.duration_s):
        t = float(k)
        sa = patient.step(circuit)
        data = ox.transmit(sa, patient.pr)
        spo2, flags = displayed(data, splitter)
        tick_t = t + TICK_OFFSET_S
        if chain is not None:
            chain.feed(data, t + RX_DELAY_S)
            bl, out = chain.tick(tick_t)
            frame = parse_bl(bl.decode("ascii").rstrip("\r\n"))
            mode, alarm = out.mode, out.alarm
        else:
            fio2 = ref.step(t, spo2 if flags == 0 else None)
            frame = BlFrame(k & 0xFFFF, int(round(fio2 * 1000)), backup_pm, 0)
        blender.on_frame(tick_t, frame)
        circuit = blender.step(t + 1.0)
        trace.append(Second(t, sa, spo2, flags, frame.fio2_pm / 1000.0, circuit, mode, alarm))
    rejected = chain.frames_rejected if chain is not None else 0
    return Run(scn, controller, trace, summarise(trace), rejected)
