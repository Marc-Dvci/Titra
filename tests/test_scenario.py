"""Scenario families and seeds (eval/scenarios.json)."""

import pytest

from bench.patient import arterial_saturation
from bench.scenario import draw, families, load, seeds


def test_six_families_with_disjoint_development_and_held_out_seeds() -> None:
    assert families() == ["stable", "moderate", "unstable", "worsening", "improving", "artefact_heavy"]
    dev, held = set(seeds("development")), set(seeds("held_out"))
    assert len(dev) == 10 and len(held) == 10 and not dev & held


def test_target_matches_the_trial_definition() -> None:
    t = load()["target"]
    assert (t["low"], t["high"], t["high_in_room_air"], t["setpoint"]) == (91, 95, 100, 93)


def test_draws_are_deterministic() -> None:
    assert draw("moderate", 3) == draw("moderate", 3)
    assert draw("moderate", 3) != draw("moderate", 4)


@pytest.mark.parametrize("family", families())
def test_drawn_parameters_lie_inside_the_family_ranges(family: str) -> None:
    cfg = load()
    fam = cfg["families"][family]
    for seed in seeds("development"):
        s = draw(family, seed, cfg)
        assert fam["shunt"][0] <= s.patient.shunt <= fam["shunt"][1]
        assert fam["vq_deficit_mmHg"][0] <= s.patient.vq_deficit_mmHg <= fam["vq_deficit_mmHg"][1]
        assert fam["apnoea_per_h"][0] <= s.patient.apnoea_per_h <= fam["apnoea_per_h"][1]
        assert fam["sensor_off_per_h"][0] <= s.oximeter.sensor_off_per_h <= fam["sensor_off_per_h"][1]
        assert s.duration_s == cfg["duration_s"]


@pytest.mark.parametrize("family", families())
def test_initial_fio2_holds_about_93_percent_at_the_start(family: str) -> None:
    s = draw(family, 0)
    p = s.patient
    sa = arterial_saturation(s.initial_fio2, p.shunt, p.vq_deficit_mmHg, p.paco2_mmHg)
    assert sa >= 0.90 or s.initial_fio2 == s.settings.fio2_max
    assert s.settings.fio2_min <= s.initial_fio2 <= s.settings.fio2_max


def test_component_streams_are_independent() -> None:
    s = draw("unstable", 1)
    assert s.rng("patient").random() == s.rng("patient").random()
    assert s.rng("patient").random() != s.rng("oximeter").random()
