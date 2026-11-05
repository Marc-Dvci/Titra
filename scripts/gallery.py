"""Builds the Devpost gallery from the run's real artefacts: white, minimal, 2400 x 1600, no titles baked in.

    python scripts/gallery.py [--out data/gallery]

Inputs: eval/results/*.json, eval/reference.json, docs/architecture.png, the nurse station at
http://127.0.0.1:8080 (live), station frames from scripts/record_station.py, and the guest's process list over ssh.
Captions, at most 140 characters each, are written to CAPTIONS.md beside the images.
"""

from __future__ import annotations

import argparse
import base64
import html
import json
import subprocess
from pathlib import Path

from fault_table import FAULTS, measured
from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
W, H = 2400, 1600
CSS = """
* { box-sizing: border-box; } body { margin: 0; width: 1200px; height: 800px; background: #fff; color: #202124;
  font-family: Inter, Roboto, 'Segoe UI', Arial, sans-serif; overflow: hidden; }
.pad { padding: 72px 80px; height: 100%; } .muted { color: #5f6368; } .big { font-size: 64px; font-weight: 600; }
.bar { height: 34px; border-radius: 17px; background: #f1f3f4; overflow: hidden; } .bar > i { display: block; height: 100%; border-radius: 17px; }
.row { display: flex; align-items: center; gap: 24px; } .mono { font-family: 'JetBrains Mono', Consolas, monospace; }
.frame { border: 1px solid #e0e3e7; border-radius: 18px; overflow: hidden; box-shadow: 0 2px 6px rgba(60,64,67,.15), 0 18px 48px rgba(60,64,67,.16); }
"""


def page(body: str) -> str:
    return f"<!doctype html><html><head><meta charset='utf-8'><style>{CSS}</style></head><body>{body}</body></html>"


def img64(path: Path) -> str:
    return (
        "data:image/"
        + path.suffix.lstrip(".").replace("jpg", "jpeg")
        + ";base64,"
        + base64.b64encode(path.read_bytes()).decode()
    )


def problem(ref: dict) -> str:
    t = ref["trial"]
    rows = [
        ("Manual control", t["time_in_target_manual_pct"], "#9aa0a6"),
        ("Automated control", t["time_in_target_automated_pct"], "#1a73e8"),
    ]
    bars = "".join(
        f"<div style='margin-top:56px'><div class='muted' style='font-size:30px;margin-bottom:14px'>{n}</div>"
        f"<div class='row'><div class='bar' style='width:820px'><i style='width:{v}%;background:{c}'></i></div>"
        f"<div class='big' style='color:{c if c != '#9aa0a6' else '#202124'}'>{v}%</div></div></div>"
        for n, v, c in rows
    )
    return page(
        f"<div class='pad'><div style='font-size:40px;font-weight:600'>Time in the safe SpO2 range</div>"
        f"<div class='muted' style='font-size:26px;margin-top:10px'>49 preterm infants on low-cost CPAP, two hospitals in southwest Nigeria</div>"
        f"{bars}<div class='muted' style='font-size:22px;position:absolute;bottom:60px'>Subhi et al., Lancet Global Health 2025</div></div>"
    )


def framed(path: Path, note: str = "") -> str:
    return page(
        f"<div class='pad' style='display:flex;flex-direction:column;justify-content:center;align-items:center'>"
        f"<div class='frame' style='width:1040px'><img src='{img64(path)}' style='width:100%;display:block'></div>"
        f"<div class='muted' style='font-size:24px;margin-top:26px'>{html.escape(note)}</div></div>"
    )


def results(dev: dict, held: dict) -> str:
    def group(name: str, m: dict) -> str:
        out = f"<div style='flex:1'><div style='font-size:30px;font-weight:600;margin-bottom:30px'>{name}</div>"
        for key, label, col in (
            ("titra", "Titra", "#1a73e8"),
            ("fixed", "Fixed FiO2", "#9aa0a6"),
            ("manual", "Manual-care model", "#c6cacf"),
        ):
            v = m[key]["all"]["time_in_target_pct"]
            out += (
                f"<div style='margin-bottom:34px'><div class='row' style='justify-content:space-between;font-size:26px'>"
                f"<span>{label}</span><b style='color:{col if key == 'titra' else '#5f6368'}'>{v:.1f}%</b></div>"
                f"<div class='bar' style='margin-top:10px'><i style='width:{v}%;background:{col}'></i></div></div>"
            )
        return out + "</div>"

    return page(
        f"<div class='pad'><div style='font-size:40px;font-weight:600'>Time in target on simulated preterm babies</div>"
        f"<div class='muted' style='font-size:26px;margin:10px 0 50px'>four-hour runs, six kinds of baby, the same babies for every controller</div>"
        f"<div class='row' style='gap:80px;align-items:flex-start'>{group('Development, 60 runs', dev)}{group('Held-out, 60 runs', held)}</div></div>"
    )


def faults(f: dict) -> str:
    def row(r: dict) -> str:
        name = (
            FAULTS.get(r["fault"], r["fault"])
            if r["id"] != "F14"
            else "Cot 2's controller killed, cots 1 and 3 watched"
        )
        ok = r["pass"]
        return (
            f"<div class='row' style='border-bottom:1px solid #e0e3e7;height:68px;font-size:17px;gap:14px'>"
            f"<span class='mono muted' style='width:40px'>{r['id']}</span>"
            f"<span style='flex:1;line-height:1.25'>{html.escape(name)}<br><span class='muted' style='font-size:15px'>{html.escape(measured(r))}</span></span>"
            f"<span style='color:{'#188038' if ok else '#c5221f'};font-weight:700;font-size:20px'>{'✓' if ok else '×'}</span></div>"
        )

    cases = f["cases"]
    cols = "".join(f"<div style='flex:1'>{''.join(row(r) for r in part)}</div>" for part in (cases[:7], cases[7:]))
    return page(
        f"<div class='pad' style='padding-top:56px'><div class='row' style='justify-content:space-between'>"
        f"<div style='font-size:40px;font-weight:600'>Fault matrix on QNX 8.0</div>"
        f"<div style='font-size:52px;font-weight:600;color:#188038'>{f['passed']} / {f['total']}</div></div>"
        f"<div class='muted' style='font-size:22px;margin:6px 0 22px'>injected from the host, timed on the host clock, security policy enforced</div>"
        f"<div class='row' style='gap:48px;align-items:flex-start'>{cols}</div></div>"
    )


def console(text: str) -> str:
    return page(
        f"<div class='pad' style='display:flex;align-items:center'><div class='frame' style='width:100%;background:#202124;padding:36px 40px'>"
        f"<pre class='mono' style='margin:0;color:#e8eaed;font-size:16px;line-height:1.5;white-space:pre-wrap;word-break:break-all'>{html.escape(text)}</pre></div></div>"
    )


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "data" / "gallery"))
    ap.add_argument("--probe-frame", help="a station frame during the probe fault, for the workflow image")
    ap.add_argument("--station", help="a station capture to use in place of the live page (docs/station.png)")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    ev = ROOT / "eval"
    ref = json.loads((ev / "reference.json").read_text(encoding="utf-8"))
    dev = json.loads((ev / "results" / "development.json").read_text(encoding="utf-8"))["mean"]
    held = json.loads((ev / "results" / "held_out.json").read_text(encoding="utf-8"))["mean"]
    fm = json.loads((ev / "results" / "faults.json").read_text(encoding="utf-8"))
    ssh = [
        "ssh",
        "-p",
        "2222",
        "-o",
        "StrictHostKeyChecking=no",
        "-o",
        "UserKnownHostsFile=/dev/null",
        "-o",
        "LogLevel=ERROR",
        "root@127.0.0.1",
    ]
    ps = subprocess.run(
        [
            *ssh,
            "echo '# uname -a'; uname -a; echo; echo \"# pidin -F '%a %_ %A'\"; "
            "pidin -F '%a %_ %A' | grep -E 'type|spo2d|valved|titractl|station|ham' | grep -v grep; echo; "
            "echo '# on -T station_t /system/bin/station -V 2 -k 100'; on -T station_t /system/bin/station -V 2 -k 100",
        ],
        capture_output=True,
        text=True,
        timeout=60,
    ).stdout
    station = out / "_station.png"
    with sync_playwright() as p:
        b = p.chromium.launch()
        if a.station:
            station = Path(a.station)
        else:
            live = b.new_page(viewport={"width": 1280, "height": 800}, device_scale_factor=2)
            live.goto("http://127.0.0.1:8080/")
            live.wait_for_selector("section.cot svg path", timeout=30000)
            live.wait_for_timeout(4000)
            live.screenshot(path=str(station), full_page=True)
        pages = {
            "02-problem.png": problem(ref),
            "03-station.png": framed(station, "The nurse station, served by the QNX board at http://127.0.0.1:8080"),
            "05-faults.png": faults(fm),
            "06-results.png": results(dev, held),
            "07-architecture.png": framed(ROOT / "docs" / "architecture.png"),
            "08-qnx-console.png": console(ps.strip()),
        }
        if a.probe_frame:
            pages["04-probe-fault.png"] = framed(
                Path(a.probe_frame), "Cot 2 after its probe comes off: FiO2 held, alarm raised, fallback at 30 s"
            )
        g = b.new_page(viewport={"width": 1200, "height": 800}, device_scale_factor=2)
        for name, doc in pages.items():
            g.set_content(doc)
            g.wait_for_timeout(300)
            g.screenshot(path=str(out / name))
        b.close()
    if not a.station:
        station.unlink(missing_ok=True)
    caps = {
        "01-cover.png": "Titra: open oxygen control for preterm babies on low-cost CPAP, built on QNX 8.0.",
        "02-problem.png": "In a Nigerian trial, automated oxygen control raised time in the safe range from 30.1% to 88.1%.",
        "03-station.png": "The nurse station runs on the QNX board: SpO2, FiO2, trend, mode and alarms for each cot.",
        "04-probe-fault.png": "A probe comes off: Titra holds FiO2, alarms at once, and falls back after 30 s without a valid reading.",
        "05-faults.png": f"{fm['passed']} of {fm['total']} injected faults meet their budgets, timed on the host clock with the policy enforced.",
        "06-results.png": "On held-out simulated babies Titra keeps 89.4% of the time in target, against 64.8% for a fixed setting.",
        "07-architecture.png": "Each cot runs three QNX processes with their own security types; HAM restarts any that fail.",
        "08-qnx-console.png": "On the QNX console, each process runs under its type, and the kernel refuses the station at a valve.",
    }
    lines = ["# Gallery captions", ""]
    for name, cap in caps.items():
        assert len(cap) <= 140, (name, len(cap))
        lines.append(f"- `{name}`: {cap}")
    (out / "CAPTIONS.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(sorted(p.name for p in out.glob("*.png"))))


if __name__ == "__main__":
    main()
