"""The decision core: plain rules that decide each outcome.

TEMPLATE: replace this example with the project's own rules. Models may read, locate or phrase; this module decides,
and every rule has a test in tests/test_core.py.

The example: a declared fact is settled only when two independent sources agree on it.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

Outcome = Literal["present", "absent", "disputed", "unverified"]
QUALITY_FLOOR = 0.6  # readings below it are refused, whatever they say


@dataclass(frozen=True)
class Observation:
    source: str  # one independent source; several readings from it count once
    says: bool  # True if the source shows the declared fact
    quality: float  # 0 to 1


@dataclass(frozen=True)
class Decision:
    outcome: Outcome
    reason: str
    counted: tuple[str, ...]  # the sources the outcome rests on, sorted


def decide(observations: list[Observation]) -> Decision:
    """Two independent sources that agree settle the fact; any disagreement disputes it; less leaves it unverified."""
    usable = [o for o in observations if o.quality >= QUALITY_FLOOR]
    claims: dict[str, set[bool]] = {}
    for o in usable:
        claims.setdefault(o.source, set()).add(o.says)
    sources = tuple(sorted(claims))
    if any(len(said) > 1 for said in claims.values()):
        return Decision("disputed", "a source contradicts itself", sources)
    said = {next(iter(v)) for v in claims.values()}
    if len(said) > 1:
        return Decision("disputed", "independent sources disagree", sources)
    if len(claims) < 2:
        return Decision("unverified", "fewer than two independent sources above the quality floor", sources)
    outcome: Outcome = "present" if said == {True} else "absent"
    return Decision(outcome, f"{len(claims)} independent sources agree", sources)
