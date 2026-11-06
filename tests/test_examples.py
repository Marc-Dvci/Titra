"""examples/ stays what the C core and the recorded answer produce."""

import importlib.util
import json
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("make_example", ROOT / "scripts" / "make_example.py")
assert _spec is not None and _spec.loader is not None
make_example = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(make_example)


@pytest.mark.parametrize("name", ["README.md", "facts.json", "prompt.txt", "grammar.gbnf", "answer.json"])
def test_an_example_file_is_what_the_code_writes(name: str) -> None:
    assert (ROOT / "examples" / name).read_text(encoding="utf-8") == make_example.build()[name]


def test_the_example_passes_the_validator() -> None:
    assert "The verdict: `ok`" in (ROOT / "examples" / "README.md").read_text(encoding="utf-8")


def test_the_grammar_of_the_example_gives_no_digit_to_the_models_sentences() -> None:
    answer = json.loads((ROOT / "examples" / "answer.json").read_text(encoding="utf-8"))
    facts = {f["cot"]: f for f in json.loads((ROOT / "examples" / "facts.json").read_text(encoding="utf-8"))}
    flagged = [n for n in answer["notes"] if facts[int(n["cot"])]["flags"]]
    assert flagged
    assert not any(ch.isdigit() for n in flagged for ch in n["why"])
