"""The evaluation runs every controller on the same babies and writes its result."""

import json
from pathlib import Path

import pytest

from bench import evaluate as ev
from bench.scenario import families, load
from bench.simrun import CONTROLLERS


@pytest.mark.integration
def test_short_evaluation_pairs_every_controller_and_writes_the_result(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    full = load()
    fam = families(full)[0]
    cfg = {**full, "duration_s": 900, "families": {fam: full["families"][fam]}, "seeds": {"development": [0]}}
    result = ev.evaluate("development", workers=1, params={}, cfg=cfg)

    assert result["runs_per_controller"] == 1
    assert {r["controller"] for r in result["runs"]} == set(CONTROLLERS)
    assert {(r["family"], r["seed"]) for r in result["runs"]} == {(fam, 0)}
    for c in CONTROLLERS:
        tit = result["mean"][c]["all"]["time_in_target_pct"]
        assert 0.0 <= tit <= 100.0
        assert result["mean"][c][fam] == result["mean"][c]["all"]
    paired = result["titra_minus_fixed_time_in_target"]
    assert paired["runs"] == 1
    assert paired["mean"] == pytest.approx(
        result["mean"]["titra"]["all"]["time_in_target_pct"] - result["mean"]["fixed"]["all"]["time_in_target_pct"]
    )
    assert result["step_tests"]

    monkeypatch.setattr(ev, "RESULTS", tmp_path)
    path = ev.write(result)
    assert path == tmp_path / "development.json"
    assert json.loads(path.read_text(encoding="utf-8"))["runs_per_controller"] == 1
