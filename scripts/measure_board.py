"""Measures the model's timing on the QNX board over ssh and writes eval/results/board.json.

    python scripts/measure_board.py     (with the guest running and the bench connected)

How long the facts and the forecast take; and the time of the board's own answers at each loop count and under each deadline,
on windows taken evenly across the held-out set, with the late ticks of every cot during each configuration.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
RESULT = ROOT / "eval" / "results" / "board.json"


class Guest:
    def __init__(self, ssh: str, key: str | None, scp: str) -> None:
        self.ssh = ssh
        self.scp_path = scp
        self.opts = ["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR"]
        self.key = ["-i", key] if key else []
        self.last_stderr = ""

    def run(self, cmd: str, timeout: float = 900.0) -> str:
        args = [self.ssh, *self.key, "-p", "2222", *self.opts, "root@127.0.0.1", cmd]
        done = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        self.last_stderr = done.stderr
        return done.stdout

    def copy(self, local: Path, remote: str) -> None:
        args = [self.scp_path, "-O", *self.key, "-P", "2222", *self.opts, str(local), f"root@127.0.0.1:{remote}"]
        subprocess.run(args, check=True, timeout=300)


def json_lines(text: str) -> list[dict[str, Any]]:
    out = []
    for line in text.splitlines():
        try:
            out.append(json.loads(line))
        except ValueError:
            continue
    return out


def load() -> dict[str, Any]:
    return json.loads(RESULT.read_text(encoding="utf-8")) if RESULT.exists() else {}


def save(data: dict[str, Any]) -> None:
    RESULT.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8")


def wait_for_the_model(guest: Guest, limit_s: float = 300.0) -> None:
    t0 = time.monotonic()
    while time.monotonic() - t0 < limit_s:
        status = json_lines(guest.run("cat /dev/llm", timeout=30.0))
        if status and status[0].get("state") == "ready":
            return
        time.sleep(5.0)
    raise SystemExit("the model did not become ready")


def timing(guest: Guest, cases: Path, set_name: str, labels: list[str], limit: int) -> None:
    """The board's own answers on a set of windows, one configuration at a time, while the cots run."""
    wait_for_the_model(guest)
    data = load()
    if "facts_and_forecast_time" not in data:
        data["facts_and_forecast_time"] = json_lines(guest.run("on -T advisor_t /system/bin/advisor -Z"))
    lines = (ROOT / "eval" / f"model_windows_{set_name}.txt").read_text(encoding="utf-8").splitlines()
    total = sum(line.startswith("case ") for line in lines)
    stride = max(total // limit, 1)
    kept: list[str] = []
    index = -1
    for line in lines:
        if line.startswith("case "):
            index += 1
        if index % stride == 0:  # evenly across the wards and the handovers
            kept.append(line)
    subset = cases.parent / "windows_subset.txt"
    subset.write_text("\n".join(kept) + "\n", encoding="utf-8")
    guest.copy(subset, "/data/windows.txt")
    subset.unlink()
    guest.run("chmod 644 /data/windows.txt")
    expected = sum(line.startswith("case ") for line in kept)
    configs = {c["label"]: c for c in json.loads(cases.read_text(encoding="utf-8"))["configurations"]}
    summary: dict[str, Any] = data.setdefault("answers_on_the_board", {}).setdefault(set_name, {})
    for label in labels:
        cfg = configs[label]
        before = [json.loads(guest.run(f"cat /dev/titra/cot{c}") or "{}") for c in (1, 2, 3)]
        t0 = time.monotonic()
        cmd = (
            f"on -T advisor_t /system/bin/advisor -r /data/windows.txt -l {cfg['min_loops']}:{cfg['max_loops']} "
            f"-D {cfg['deadline_ms']}"
        )
        raw = guest.run(cmd, timeout=7000.0)
        rows = json_lines(raw)
        if len(rows) != expected:
            # Kept outside the repo: a run that did not finish is not a result, but it is the evidence of why.
            kept_raw = Path(tempfile.gettempdir()) / f"titra_{set_name}_{label}.txt"
            kept_raw.write_text(raw + "\n--- stderr\n" + guest.last_stderr, encoding="utf-8")
            raise SystemExit(f"{label}: {len(rows)} answers of {expected}; not a result; raw output in {kept_raw}")
        after = [json.loads(guest.run(f"cat /dev/titra/cot{c}") or "{}") for c in (1, 2, 3)]
        out = ROOT / "eval" / "results" / f"board_sample_{set_name}_{label}.jsonl"
        out.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")
        asked = [r for r in rows if r["validator"] != "not asked"]
        generated = sum(int(r["generated"]) for r in rows)
        prompt_tokens = sum(int(r["prompt_tokens"]) for r in rows)
        gen_s = sum(float(r["gen_ms"]) for r in rows) / 1000.0
        prompt_s = sum(float(r["prompt_ms"]) for r in rows) / 1000.0
        summary[label] = {
            "answers": len(asked),
            "not_asked": len(rows) - len(asked),
            "valid": sum(r["validator"] == "ok" for r in rows),
            "gen_tok_s": round(generated / gen_s, 2) if gen_s else 0.0,
            "prompt_tok_s": round(prompt_tokens / prompt_s, 1) if prompt_s else 0.0,
            "mean_answer_s": round(sum(float(r["total_ms"]) for r in asked) / max(len(asked), 1) / 1000.0, 1),
            "longest_answer_s": round(max((float(r["total_ms"]) for r in asked), default=0.0) / 1000.0, 1),
            "deadlines_met": sum(int(r["deadline_met"]) for r in asked),
            "deadline_ms": cfg["deadline_ms"],
            "tokens_at_loops": [sum(int(r["loops"][k]) for r in rows) for k in range(4)],
            "late_ticks_during": [
                int(a.get("late_ticks", -1)) - int(b.get("late_ticks", 0)) for a, b in zip(after, before, strict=True)
            ],
            "longest_tick_interval_s": [a.get("max_interval") for a in after],
            "seconds": round(time.monotonic() - t0),
        }
        print(label, summary[label], flush=True)
        save(data)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ssh", default="ssh")
    ap.add_argument("--scp", default="scp")
    ap.add_argument("--ssh-key")
    ap.add_argument("--limit", type=int, default=7, help="how many windows the board replays per configuration")
    ap.add_argument("--config", action="append", help="a configuration from eval/model_cases.json; default all")
    ap.add_argument("--set", dest="set_name", default="held_out", choices=("development", "held_out"))
    ap.add_argument("--cases", type=Path, default=ROOT / "eval" / "model_cases.json")
    a = ap.parse_args()
    guest = Guest(a.ssh, a.ssh_key, a.scp)
    labels = a.config or [c["label"] for c in json.loads(a.cases.read_text(encoding="utf-8"))["configurations"]]
    timing(guest, a.cases, a.set_name, labels, a.limit)


if __name__ == "__main__":
    main()
