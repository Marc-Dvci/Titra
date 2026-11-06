"""Measures the model on the QNX board over ssh and writes eval/results/board.json.

    python scripts/measure_board.py placement --bench qnx/build/x86_64/llm_bench      (boot the guest with 12 GB for this)
    python scripts/measure_board.py timing                                            (the image as built)

placement: the same prompt and greedy sampling with the weights read into the process, mapped from an ordinary shared
memory object, and mapped from the physically contiguous object llmstore makes; and the compute pool's spin before it sleeps.
timing: how long the facts and the forecast take; and the board's own answers with their timings, at each loop count and
under each deadline, on the held-out windows, with the late ticks of every cot during each configuration.
"""

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import time
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
RESULT = ROOT / "eval" / "results" / "board.json"
WINDOWS = ROOT / "eval" / "model_windows_held_out.txt"


class Guest:
    def __init__(self, ssh: str, key: str | None, scp: str) -> None:
        self.ssh = ssh
        self.scp_path = scp
        self.opts = ["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "LogLevel=ERROR"]
        self.key = ["-i", key] if key else []

    def run(self, cmd: str, timeout: float = 900.0) -> str:
        args = [self.ssh, *self.key, "-p", "2222", *self.opts, "root@127.0.0.1", cmd]
        return subprocess.run(args, capture_output=True, text=True, timeout=timeout).stdout

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


def bench(
    guest: Guest, model: str, mode: str, loops: str = "1,4", poll: int = 50, tokens: int = 48
) -> list[dict[str, Any]]:
    cmd = f"/tmp/llm_bench -m {shlex.quote(model)} -t 3 -f 1 -L {loops} -n {tokens} -M {mode} -w {poll} -X"
    return json_lines(guest.run(cmd))


def placement(guest: Guest, binary: Path) -> None:
    guest.copy(binary, "/tmp/llm_bench")
    guest.run("chmod +x /tmp/llm_bench")
    data = load()
    guest.run("/system/bin/llmstore -C -i /models/dory.gguf -n plain.gguf")
    runs: dict[str, Any] = {}
    # Twice each, in turn, so a slow spell on the host does not favour one placement.
    for attempt in (1, 2):
        for name, model, mode in (
            ("read_into_process", "/models/dory.gguf", "read"),
            ("mapped_shared_memory", "/dev/shmem/plain.gguf", "mmap"),
            ("mapped_contiguous_shared_memory", "/dev/shmem/dory.gguf", "mmap"),
        ):
            runs.setdefault(name, []).append(bench(guest, model, mode))
            print(attempt, name, runs[name][-1], flush=True)
    data["weights_placement"] = runs
    pools: dict[str, Any] = {}
    for poll in (0, 50, 200):
        pools[str(poll)] = bench(guest, "/dev/shmem/dory.gguf", "mmap", poll=poll)
        print("poll", poll, pools[str(poll)], flush=True)
    data["pool_poll"] = pools
    guest.run("rm -f /dev/shmem/plain.gguf")
    save(data)


def timing(guest: Guest, cases: Path, labels: list[str], limit: int) -> None:
    """The board's own answers on the held-out windows, one configuration at a time, while the cots run."""
    data = load()
    if "facts_and_forecast_time" not in data:
        data["facts_and_forecast_time"] = json_lines(guest.run("on -T advisor_t /system/bin/advisor -Z"))
    lines = WINDOWS.read_text(encoding="utf-8").splitlines()
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
    configs = {c["label"]: c for c in json.loads(cases.read_text(encoding="utf-8"))["configurations"]}
    summary: dict[str, Any] = data.setdefault("answers_on_the_board", {})
    for label in labels:
        cfg = configs[label]
        before = [json.loads(guest.run(f"cat /dev/titra/cot{c}") or "{}") for c in (1, 2, 3)]
        t0 = time.monotonic()
        cmd = (
            f"on -T advisor_t /system/bin/advisor -r /data/windows.txt -l {cfg['min_loops']}:{cfg['max_loops']} "
            f"-D {cfg['deadline_ms']}"
        )
        rows = json_lines(guest.run(cmd, timeout=7000.0))
        after = [json.loads(guest.run(f"cat /dev/titra/cot{c}") or "{}") for c in (1, 2, 3)]
        out = ROOT / "eval" / "results" / f"model_outputs_held_out_{label}.jsonl"
        out.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")
        generated = sum(int(r["generated"]) for r in rows)
        prompt_tokens = sum(int(r["prompt_tokens"]) for r in rows)
        gen_s = sum(float(r["gen_ms"]) for r in rows) / 1000.0
        prompt_s = sum(float(r["prompt_ms"]) for r in rows) / 1000.0
        summary[label] = {
            "answers": len(rows),
            "valid": sum(r["validator"] == "ok" for r in rows),
            "gen_tok_s": round(generated / gen_s, 2) if gen_s else 0.0,
            "prompt_tok_s": round(prompt_tokens / prompt_s, 1) if prompt_s else 0.0,
            "mean_answer_s": round(sum(float(r["total_ms"]) for r in rows) / max(len(rows), 1) / 1000.0, 1),
            "longest_answer_s": round(max((float(r["total_ms"]) for r in rows), default=0.0) / 1000.0, 1),
            "deadlines_met": sum(int(r["deadline_met"]) for r in rows),
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
    ap.add_argument("part", choices=("placement", "timing"))
    ap.add_argument("--bench", type=Path, default=ROOT / "qnx" / "build" / "x86_64" / "llm_bench")
    ap.add_argument("--ssh", default="ssh")
    ap.add_argument("--scp", default="scp")
    ap.add_argument("--ssh-key")
    ap.add_argument("--limit", type=int, default=70, help="how many windows the board replays per configuration")
    ap.add_argument("--config", action="append", help="a configuration from eval/model_cases.json; default all")
    ap.add_argument("--cases", type=Path, default=ROOT / "eval" / "model_cases.json")
    a = ap.parse_args()
    guest = Guest(a.ssh, a.ssh_key, a.scp)
    if a.part == "placement":
        placement(guest, a.bench)
    else:
        labels = a.config or [c["label"] for c in json.loads(a.cases.read_text(encoding="utf-8"))["configurations"]]
        timing(guest, a.cases, labels, a.limit)


if __name__ == "__main__":
    main()
