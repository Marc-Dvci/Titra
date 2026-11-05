"""Prints the fault matrix results (eval/results/faults.json) as a Markdown table for the README.

python scripts/fault_table.py [eval/results/faults.json]
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

FAULTS = {
    "sensor_off": "Oximeter flags sensor off",
    "cable_cut": "Oximeter cable cut",
    "repeat_frame": "Oximeter repeats its last frame",
    "frozen_values": "SpO2, pulse and perfusion frozen",
    "garbage_line": "Noise and corrupt frames on the line",
    "kill_spo2d": "Oximeter driver killed",
    "kill_titractl": "Controller killed",
    "stop_titractl": "Controller hung (stopped)",
    "kill_valved": "Blender driver killed",
    "blender_link_cut": "Blender link cut",
    "bad_command": "Out-of-bounds commands to the blender driver",
    "open_valve_from_other_type": "Nurse station's type opens a blender driver",
    "cpu_load": "Every core busy at lower priority, 300 s",
}


def budget(expect: dict[str, object]) -> str:
    parts = []
    for key, label in (("alarm_within_s", "alarm"), ("fallback_within_s", "fallback"), ("resume_within_s", "resume")):
        if key in expect:
            parts.append(f"{label} {expect[key]} s")
    if expect.get("no_step_at_resume"):
        parts.append("no step")
    if expect.get("no_fallback"):
        parts.append("no fallback")
    if "refused_fraction" in expect:
        parts.append("100% refused")
    if "missed_ticks" in expect or "missed_ticks_on_watched_cots" in expect:
        parts.append("0 missed ticks")
    return ", ".join(parts)


def measured(r: dict[str, object]) -> str:
    m = r["measured"]
    assert isinstance(m, dict)
    parts = []
    expect = r["expect"]
    assert isinstance(expect, dict)
    for key, label, budget_key in (
        ("alarm_s", "alarm", "alarm_within_s"),
        ("fallback_s", "fallback", "fallback_within_s"),
        ("resume_s", "resume", "resume_within_s"),
    ):
        if key in m and budget_key in expect:
            parts.append(f"{label} {m[key]:.1f} s")
    if "step_at_resume" in m:
        parts.append(f"step {m['step_at_resume']:.2f}")
    if "refused" in m:
        parts.append(f"{m['refused']} of {m['attempts']} refused")
    if "late_ticks" in m:
        late = m["late_ticks"]
        assert isinstance(late, dict)
        parts.append(f"{sum(int(v) for v in late.values())} missed ticks")
    if r["fault"] == "garbage_line":
        parts.append("no fallback" if not m.get("fallback_seen") else "fallback")
    return ", ".join(parts)


def main() -> None:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "eval" / "results" / "faults.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    print("| ID | Fault | Budget | Measured on the host clock | Result |")
    print("|---|---|---|---|---|")
    for r in data["cases"]:
        name = FAULTS.get(r["fault"], r["fault"])
        if r["id"] == "F14":
            name = "Controller of cot 2 killed: cots 1 and 3 watched"
        print(f"| {r['id']} | {name} | {budget(r['expect'])} | {measured(r)} | {'pass' if r['pass'] else 'fail'} |")
    print(f"\n{data['passed']} of {data['total']} cases pass. Target: {data['target']}.")


if __name__ == "__main__":
    main()
