"""The rules in app/core.py: one test per rule and per boundary, and the cases that must decide nothing."""

from hypothesis import given
from hypothesis import strategies as st

from app.core import QUALITY_FLOOR, Observation, decide


def obs(source: str, says: bool = True, quality: float = 0.9) -> Observation:
    return Observation(source, says, quality)


def test_two_independent_sources_that_agree_settle_present() -> None:
    d = decide([obs("satellite"), obs("registry")])
    assert (d.outcome, d.counted) == ("present", ("registry", "satellite"))


def test_two_independent_sources_that_agree_settle_absent() -> None:
    assert decide([obs("satellite", False), obs("registry", False)]).outcome == "absent"


def test_one_source_alone_leaves_it_unverified() -> None:
    assert decide([obs("satellite")]).outcome == "unverified"


def test_two_readings_from_one_source_count_once() -> None:
    assert decide([obs("satellite"), obs("satellite")]).outcome == "unverified"


def test_independent_sources_that_disagree_dispute_it() -> None:
    assert decide([obs("satellite"), obs("registry", False)]).outcome == "disputed"


def test_a_source_that_contradicts_itself_disputes_it() -> None:
    d = decide([obs("satellite"), obs("satellite", False), obs("registry")])
    assert (d.outcome, d.reason) == ("disputed", "a source contradicts itself")


def test_a_reading_at_the_quality_floor_counts() -> None:
    assert decide([obs("satellite", quality=QUALITY_FLOOR), obs("registry")]).outcome == "present"


def test_a_reading_just_below_the_quality_floor_is_refused() -> None:
    d = decide([obs("satellite", quality=QUALITY_FLOOR - 0.01), obs("registry")])
    assert (d.outcome, d.counted) == ("unverified", ("registry",))


def test_a_refused_reading_cannot_dispute() -> None:
    assert decide([obs("satellite"), obs("registry"), obs("photo", False, quality=0.1)]).outcome == "present"


def test_nothing_observed_decides_nothing() -> None:
    assert decide([]).outcome == "unverified"


readings = st.lists(st.builds(Observation, st.sampled_from(["a", "b", "c"]), st.booleans(), st.floats(0, 1)))


@given(readings)
def test_repeating_every_reading_changes_nothing(observations: list[Observation]) -> None:
    assert decide(observations * 2) == decide(observations)


@given(readings)
def test_the_order_of_readings_does_not_matter(observations: list[Observation]) -> None:
    assert decide(list(reversed(observations))) == decide(observations)
