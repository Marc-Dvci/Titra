"""The on-board model's evaluation: the case windows, and the scoring of what the model wrote.

    python -m bench model-cases development     write the windows the board replays (eval/model_windows_development.txt)
    python -m bench model-score FILE.jsonl      score recorded answers against their windows

The board replays the windows through /dev/llm with `advisor -r` and records one JSON line per answer. Scoring runs the
answers again through the same C validator the board uses, so CI re-validates every recorded output with no model and
no QNX. Facts, flags and ground truth come from the C core and the simulator.
"""

from __future__ import annotations

import ctypes
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from bench.core import lib as core_lib
from bench.flags import FLAGS, FactsC, compute, default_thresholds, flag_names, to_seconds
from bench.scenario import draw
from bench.simrun import run

ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT / "eval" / "model_cases.json"
FIELDS = (
    "cot",
    "seconds",
    "valid_seconds",
    "time_in_target_pct",
    "above_95_on_o2_pct",
    "below_85_pct",
    "desat_episodes",
    "longest_desat_s",
    "fio2_early_pm",
    "fio2_late_pm",
    "fio2_max_pm",
    "probe_events",
    "invalid_pct",
    "hold_s",
    "fallback_s",
    "restarts",
    "fallback_now",
    "risk_pct",
    "flags",
    "priority",
)


@dataclass
class Window:
    id: str
    kind: str
    facts: list[dict[str, int]]


def _validator() -> ctypes.CDLL:
    lib = core_lib()
    lib.ta_watch_size.restype = ctypes.c_size_t
    lib.ta_result_name.restype = ctypes.c_char_p
    lib.ta_result_name.argtypes = [ctypes.c_int]
    for name in ("ta_validate_watch", "ta_validate_handover"):
        fn = getattr(lib, name)
        fn.restype = ctypes.c_int
        fn.argtypes = [ctypes.c_char_p, ctypes.POINTER(FactsC), ctypes.c_int, ctypes.c_void_p]
    return lib


def validate(text: str, facts: list[dict[str, int]], kind: str) -> str:
    """The C validator's verdict on an answer."""
    lib = _validator()
    arr = (FactsC * len(facts))()
    for i, f in enumerate(facts):
        for k, v in f.items():
            setattr(arr[i], k, v)
    out = ctypes.create_string_buffer(lib.ta_watch_size())
    fn = lib.ta_validate_handover if kind == "handover" else lib.ta_validate_watch
    code = fn(text.encode("utf-8"), arr, len(facts), out)
    return str(lib.ta_result_name(code).decode("ascii"))


def build_windows(set_name: str, cfg: dict[str, Any] | None = None) -> list[Window]:
    cfg = cfg or json.loads(CASES.read_text(encoding="utf-8"))
    th = default_thresholds()
    cache: dict[tuple[str, int], ctypes.Array[Any]] = {}

    def seconds(family: str, seed: int) -> ctypes.Array[Any]:
        key = (family, seed)
        if key not in cache:
            cache[key] = to_seconds(run(draw(family, seed), "titra").trace)
        return cache[key]

    def facts_of(ward: list[str], seed: int, end_s: int, window_s: int) -> list[dict[str, int]]:
        out = []
        for i, family in enumerate(ward):
            # Each cot is a different baby: the seed moves on with the cot.
            f = compute(seconds(family, seed + i * 100), end_s, window_s, cot=i + 1, thresholds=th)
            out.append({name: int(getattr(f, name)) for name in FIELDS})
        return out

    windows: list[Window] = []
    for w, ward in enumerate(cfg["wards"]):
        for seed in cfg["seeds"][set_name]:
            for end_s in cfg["end_s"]:
                windows.append(
                    Window(
                        f"w{w}-s{seed}-e{end_s}", "watch", facts_of(ward, int(seed), int(end_s), int(cfg["window_s"]))
                    )
                )
    for w, ward in enumerate(cfg["handover_wards"]):
        for seed in cfg["seeds"][set_name]:
            windows.append(
                Window(f"h{w}-s{seed}", "handover", facts_of(ward, int(seed), 14400, int(cfg["handover_window_s"])))
            )
    return windows


def windows_text(windows: list[Window]) -> str:
    """The plain format `advisor -r` reads."""
    lines = []
    for win in windows:
        lines.append(f"case {win.id} {win.kind} {len(win.facts)}")
        for f in win.facts:
            lines.append(" ".join(str(f[name]) for name in FIELDS))
    return "\n".join(lines) + "\n"


def read_windows(path: Path) -> dict[str, Window]:
    out: dict[str, Window] = {}
    lines = path.read_text(encoding="utf-8").splitlines()
    i = 0
    while i < len(lines):
        _, wid, kind, n = lines[i].split()
        facts = [dict(zip(FIELDS, map(int, lines[i + 1 + k].split()), strict=True)) for k in range(int(n))]
        out[wid] = Window(wid, kind, facts)
        i += 1 + int(n)
    return out


def write_windows(set_name: str) -> Path:
    path = ROOT / "eval" / f"model_windows_{set_name}.txt"
    path.write_text(windows_text(build_windows(set_name)), encoding="utf-8")
    return path


def order_agreement(order: list[int], facts: list[dict[str, int]]) -> float:
    """Kendall's tau between the model's order and the deterministic priority. Ties in priority count as agreement."""
    rank = {f["cot"]: f["priority"] for f in facts}
    agree = total = 0
    for i in range(len(order)):
        for j in range(i + 1, len(order)):
            total += 1
            if rank[order[i]] >= rank[order[j]]:
                agree += 1
    return (2 * agree - total) / total if total else 1.0


def flag_mentioned(flag: str, text: str, words: dict[str, list[str]]) -> bool:
    low = text.lower()
    return any(w in low for w in words.get(flag, []))


def score_answer(row: dict[str, Any], window: Window, words: dict[str, list[str]]) -> dict[str, Any]:
    verdict = validate(row.get("text", ""), window.facts, window.kind)
    out: dict[str, Any] = {
        "id": row["id"],
        "kind": window.kind,
        "validator": verdict,
        "flags_raised": 0,
        "flags_covered": 0,
    }
    if verdict != "ok":
        return out
    parsed = json.loads(row["text"])
    notes = parsed.get("notes") or parsed.get("cots")
    by_cot = {int(n["cot"]): n.get("why") or n.get("summary") or "" for n in notes}
    for f in window.facts:
        for name in flag_names(f["flags"]):
            out["flags_raised"] += 1
            if flag_mentioned(name, by_cot.get(f["cot"], ""), words):
                out["flags_covered"] += 1
    if window.kind == "watch":
        out["order_tau"] = round(order_agreement([int(n["cot"]) for n in notes], window.facts), 3)
        first = int(notes[0]["cot"])
        top = max(f["priority"] for f in window.facts)
        out["first_is_top_priority"] = next(f["priority"] for f in window.facts if f["cot"] == first) == top
    return out


def score_file(path: Path, windows: dict[str, Window], cases_path: Path = CASES) -> dict[str, Any]:
    words = json.loads(cases_path.read_text(encoding="utf-8"))["flag_words"]
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    scored = [score_answer(r, windows[r["id"]], words) for r in rows]
    n = len(scored)
    valid = [s for s in scored if s["validator"] == "ok"]
    verdicts: dict[str, int] = {}
    for s in scored:
        verdicts[s["validator"]] = verdicts.get(s["validator"], 0) + 1
    raised = sum(s["flags_raised"] for s in valid)
    covered = sum(s["flags_covered"] for s in valid)
    taus = [s["order_tau"] for s in valid if "order_tau" in s]
    firsts = [s["first_is_top_priority"] for s in valid if "first_is_top_priority" in s]
    out = {
        "file": path.name,
        "answers": n,
        "valid": len(valid),
        "valid_rate": round(len(valid) / max(n, 1), 3),
        "verdicts": verdicts,
        "flags_raised": raised,
        "flags_covered": covered,
        "flag_coverage": round(covered / max(raised, 1), 3),
        "order_tau_mean": round(sum(taus) / max(len(taus), 1), 3),
        "first_is_top_priority_rate": round(sum(firsts) / max(len(firsts), 1), 3),
        "generated_tokens": sum(int(r.get("generated", 0)) for r in rows),
    }
    timed = [r for r in rows if "total_ms" in r]
    if timed:
        gen_ms = sum(float(r.get("gen_ms", 0.0)) for r in timed)
        gen_tok = sum(int(r.get("generated", 0)) for r in timed)
        out["gen_tok_s"] = round(gen_tok / (gen_ms / 1000.0), 2) if gen_ms else 0.0
        out["mean_total_s"] = round(sum(float(r["total_ms"]) for r in timed) / len(timed) / 1000.0, 1)
        out["deadlines_met"] = sum(int(r.get("deadline_met", 0)) for r in timed)
    return out


FLAG_NAMES = tuple(FLAGS.values())
