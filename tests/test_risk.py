"""The early-warning model: its features through the C core, its scoring, and its ground truth."""

import json
from pathlib import Path

import pytest

from bench import risk
from bench.flags import to_seconds
from bench.metrics import Second

ROOT = Path(__file__).resolve().parents[1]


def trace(n: int, spo2: int = 93, fio2: float = 0.30, sa: float = 0.93) -> list[Second]:
    return [Second(float(i), sa, spo2, 0, fio2, fio2, 0, 0) for i in range(n)]


def test_features_of_a_calm_hour() -> None:
    x = risk.features(to_seconds(trace(3600)), 3600)
    assert len(x) == risk.N_FEATURES
    assert x[0] == pytest.approx(93.0)
    assert x[1] == pytest.approx(0.0, abs=1e-6)
    assert x[2] == 93.0
    assert x[3] == 0.0
    assert x[5] == 0.0
    assert x[6] == 0.0
    assert x[7] == pytest.approx(0.30)
    assert x[8] == pytest.approx(0.0, abs=1e-9)
    assert x[11] == 0.0
    assert x[12] == 3600.0


def test_a_recent_dip_shows_in_its_features() -> None:
    t = trace(3600)
    t[3500:3520] = [Second(float(i), 0.80, 80, 0, 0.3, 0.3, 0, 0) for i in range(3500, 3520)]
    x = risk.features(to_seconds(t), 3600)
    assert x[5] == 1.0
    assert x[6] == 1.0
    assert x[2] == 80.0
    assert x[12] == pytest.approx(80.0)


def test_an_old_dip_counts_for_the_hour_only() -> None:
    t = trace(3600)
    t[100:120] = [Second(float(i), 0.80, 80, 0, 0.3, 0.3, 0, 0) for i in range(100, 120)]
    x = risk.features(to_seconds(t), 3600)
    assert x[5] == 0.0
    assert x[6] == 1.0


def test_a_rise_in_fio2_shows_as_a_change() -> None:
    t = trace(2700) + trace(900, fio2=0.40)
    assert risk.features(to_seconds(t), 3600)[8] == pytest.approx(0.0, abs=1e-9)
    t = trace(3000) + trace(600, fio2=0.40)
    assert risk.features(to_seconds(t), 3600)[8] == pytest.approx(0.10)


def test_no_valid_reading_is_reported_not_guessed() -> None:
    t = [Second(float(i), 0.93, None, 0, 0.30, 0.30, 0, 0) for i in range(1000)]
    x = risk.features(to_seconds(t), 1000)
    assert x[11] == 100.0
    assert x[0] == 0.0


def test_dips_begin_where_true_saturation_falls() -> None:
    t = trace(100)
    t[20:35] = [Second(float(i), 0.80, 93, 0, 0.3, 0.3, 0, 0) for i in range(20, 35)]
    t[50:55] = [Second(float(i), 0.80, 93, 0, 0.3, 0.3, 0, 0) for i in range(50, 55)]
    assert risk.dip_starts(t, 0.85, 10) == [20]


def test_auroc_of_perfect_reversed_and_tied_scores() -> None:
    assert risk.auroc([0.1, 0.2, 0.8, 0.9], [0, 0, 1, 1]) == 1.0
    assert risk.auroc([0.9, 0.8, 0.2, 0.1], [0, 0, 1, 1]) == 0.0
    assert risk.auroc([0.5, 0.5, 0.5, 0.5], [0, 0, 1, 1]) == 0.5
    assert risk.auroc([0.1, 0.2], [0, 0]) == 0.5


def rows(labels: list[int]) -> list[risk.Row]:
    return [risk.Row("stable", 0, i, [0.0] * risk.N_FEATURES, y, -1) for i, y in enumerate(labels)]


def test_alerts_are_counted_at_the_agreed_rate() -> None:
    r = rows([1, 1, 0, 0] * 15)
    scores = [1.0, 0.9, 0.1, 0.0] * 15
    at = risk.caught_at_alert_rate(r, scores, 2.0, 60)
    assert at["alert_minutes"] == 2
    assert at["true_alerts"] == 2
    assert at["precision"] == 1.0
    assert at["recall"] == pytest.approx(round(2 / 30, 3))


def test_baselines_read_their_own_features() -> None:
    r = [risk.Row("stable", 0, 0, [float(i) for i in range(risk.N_FEATURES)], 0, -1)]
    assert risk.baseline_scores(r, "dips_85_last_hour") == [6.0]
    assert risk.baseline_scores(r, "time_below_91_last_15_min") == [3.0]
    assert risk.baseline_scores(r, "spo2_sd_last_15_min") == [1.0]


def test_the_score_is_a_probability() -> None:
    assert 0.0 <= risk.score([93.0, 1.0, 90.0, 1.0, 0.0, 0.0, 0.0, 0.3, 0.0, 0.0, 2.0, 0.0, 3600.0]) <= 1.0


def test_more_dips_score_at_least_as_high_for_a_troubled_baby() -> None:
    calm = [93.0, 1.0, 91.0, 0.0, 0.0, 0.0, 0.0, 0.25, 0.0, 0.0, 2.0, 0.0, 3600.0]
    troubled = [88.0, 5.0, 70.0, 45.0, 12.0, 3.0, 8.0, 0.45, 0.05, 0.05, 2.0, 0.0, 60.0]
    assert risk.score(troubled) > risk.score(calm)


def test_the_cases_keep_their_seeds_apart() -> None:
    cfg = json.loads(risk.CASES.read_text(encoding="utf-8"))
    assert not set(cfg["seeds"]["development"]) & set(cfg["seeds"]["held_out"])
    assert cfg["horizon_s"] == 600
