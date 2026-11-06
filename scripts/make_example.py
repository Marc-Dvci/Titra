"""Writes examples/: one unit-watch answer from start to finish, from a recorded held-out answer and the C core.

python scripts/make_example.py

The facts are the case window's, the prompt and the grammar are what the C core composes for them, the answer is the one
recorded for that window, and the verdict is the C validator's. tests/test_examples.py checks the files stay what this writes.
"""

from __future__ import annotations

import ctypes
import json
from pathlib import Path

from bench.core import lib as core_lib
from bench.flags import FactsC, flag_names
from bench.model_eval import read_windows, validate

ROOT = Path(__file__).resolve().parents[1]
WINDOW_ID = "w0-s1000-e10800"
ANSWERS = "eval/results/model_outputs_held_out_L1.jsonl"
WINDOWS = "eval/model_windows_held_out.txt"
BUFFER = 32768


def composed(facts: list[dict[str, int]]) -> tuple[str, str, list[str]]:
    """The prompt, the grammar and each cot's record, from the C core."""
    lib = core_lib()
    arr = (FactsC * len(facts))()
    for i, f in enumerate(facts):
        for k, v in f.items():
            setattr(arr[i], k, v)
    prompt = ctypes.create_string_buffer(BUFFER)
    grammar = ctypes.create_string_buffer(BUFFER)
    record = ctypes.create_string_buffer(1024)
    for name in ("ta_watch_prompt", "ta_watch_grammar"):
        getattr(lib, name).restype = ctypes.c_int
    lib.ta_watch_prompt.argtypes = [
        ctypes.POINTER(FactsC),
        ctypes.c_int,
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_size_t,
    ]
    lib.ta_watch_grammar.argtypes = [ctypes.POINTER(FactsC), ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t]
    lib.ta_handover_facts.restype = ctypes.c_int
    lib.ta_handover_facts.argtypes = [ctypes.POINTER(FactsC), ctypes.c_char_p, ctypes.c_size_t]
    assert lib.ta_watch_prompt(arr, len(facts), None, prompt, BUFFER) > 0
    assert lib.ta_watch_grammar(arr, len(facts), grammar, BUFFER) > 0
    records = []
    for i in range(len(facts)):
        assert lib.ta_handover_facts(ctypes.byref(arr[i]), record, 1024) > 0
        records.append(record.value.decode("utf-8"))
    return prompt.value.decode("utf-8"), grammar.value.decode("utf-8"), records


def page(facts: list[dict[str, int]], answer: dict[str, object], verdict: str, records: list[str], tokens: int) -> str:
    rows = {f["cot"]: f for f in facts}
    by_cot = dict(zip([f["cot"] for f in facts], records, strict=True))
    lines = [
        "# One answer, from start to finish",
        "",
        f"A held-out case window (`{WINDOW_ID}`): a ward of three simulated babies, the last three hours. Everything here is",
        "simulated. Written by [`scripts/make_example.py`](../scripts/make_example.py) from the C core and the recorded answer,",
        "and checked against them by [`tests/test_examples.py`](../tests/test_examples.py).",
        "",
        "| Step | File | Written by |",
        "|---|---|---|",
        "| The facts and flags of each cot | [`facts.json`](facts.json) | C, from the controller's status |",
        "| The prompt | [`prompt.txt`](prompt.txt) | C, from the facts |",
        "| The grammar for this request: each cot's words, actions and order | [`grammar.gbnf`](grammar.gbnf) | C, from the flags |",
        f"| The answer, {tokens} tokens | [`answer.json`](answer.json) | The model, inside that grammar |",
        f"| The verdict: `{verdict}` | | C, the validator, trusting nothing from the grammar |",
        "",
        "## What the nurse reads",
        "",
    ]
    notes = answer["notes"]
    assert isinstance(notes, list)
    for i, note in enumerate(notes, start=1):
        f = rows[int(note["cot"])]
        flags = ", ".join(flag_names(f["flags"])) or "none"
        risk = f" Chance of a dip in 10 minutes: {f['risk_pct']}%." if f["risk_pct"] >= 0 else ""
        action = str(note["action"])
        lines += [
            f"**{i}. Cot {note['cot']}.** {action[0].upper() + action[1:]}. {note['why']}{risk}",
            "",
            f"Flags, from C: {flags}. Record, from C: {by_cot[int(note['cot'])]}",
            "",
        ]
    lines += [
        "The sentences of flagged cots are the model's; cot 1 has no flag, so its sentence is the rules' own and the model",
        "wrote nothing for it. Every number on the page comes from C.",
    ]
    return "\n".join(lines) + "\n"


def build() -> dict[str, str]:
    window = read_windows(ROOT / WINDOWS)[WINDOW_ID]
    rows = (json.loads(x) for x in (ROOT / ANSWERS).read_text(encoding="utf-8").splitlines())
    row = next(r for r in rows if r["id"] == WINDOW_ID)
    prompt, grammar, records = composed(window.facts)
    verdict = validate(row["text"], window.facts, window.kind)
    answer = json.loads(row["text"])
    facts_out = [{**f, "flag_names": list(flag_names(f["flags"]))} for f in window.facts]
    return {
        "README.md": page(window.facts, answer, verdict, records, int(row["generated"])),
        "facts.json": json.dumps(facts_out, indent=1) + "\n",
        "prompt.txt": prompt,
        "grammar.gbnf": grammar,
        "answer.json": json.dumps(answer, indent=1) + "\n",
    }


def main() -> None:
    out = ROOT / "examples"
    out.mkdir(exist_ok=True)
    for name, text in build().items():
        (out / name).write_text(text, encoding="utf-8", newline="\n")
        print(out / name)


if __name__ == "__main__":
    main()
