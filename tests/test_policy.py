"""The security-policy checker (qnx/image/check_policy.py), and the committed policy when it exists."""

import importlib.util
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("check_policy", ROOT / "qnx" / "image" / "check_policy.py")
assert spec and spec.loader
check_policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check_policy)

GOOD = """
type titractl1_t; type valved1_t; type spo2d1_t; type station_t;
allow titractl1_t valved1_t:channel connect;
allow titractl1_t self:ability { channel_connect:spo2d1_t, devb_t setuid:0 };
allow station_t titractl1_t:channel connect;  # the station reads status only
"""


def test_both_rule_forms_are_read() -> None:
    pairs = check_policy.connections(GOOD)
    assert ("titractl1_t", "valved1_t") in pairs
    assert ("titractl1_t", "spo2d1_t") in pairs
    assert ("titractl1_t", "devb_t") in pairs
    assert ("station_t", "titractl1_t") in pairs


def test_comments_do_not_count() -> None:
    assert check_policy.connections("# allow station_t valved1_t:channel connect;") == set()


def test_a_policy_with_each_controller_on_its_own_drivers_passes() -> None:
    assert check_policy.violations(GOOD, cots=1) == []


@pytest.mark.parametrize(
    "rule",
    [
        "allow station_t valved1_t:channel connect;",
        "allow titractl2_t valved1_t:channel connect;",
        "allow station_t self:ability { channel_connect:spo2d1_t };",
    ],
)
def test_any_other_type_reaching_a_driver_is_a_violation(rule: str) -> None:
    problems = check_policy.violations(GOOD + rule, cots=1)
    assert problems and "may connect to" in problems[0]


def test_a_controller_without_its_blender_rule_is_reported() -> None:
    problems = check_policy.violations("allow station_t titractl1_t:channel connect;", cots=1)
    assert problems == ["titractl1_t has no rule to reach valved1_t"]


def test_committed_policy_isolates_every_cot() -> None:
    policy = ROOT / "qnx" / "image" / "titra.secpol"
    if not policy.exists():
        pytest.skip("reason: the policy is generated on the QNX target and committed after review")
    assert check_policy.violations(policy.read_text(encoding="utf-8")) == []
