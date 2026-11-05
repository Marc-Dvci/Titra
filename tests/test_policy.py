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


MODEL = (
    GOOD
    + """
type advisor_t; type llm_t;
allow advisor_t self:ability { channel_connect:titractl1_t,llm_t,devb_t };
allow llm_t self:ability { pathspace prot_exec channel_connect:devb_t,ham_t };
"""
)


def test_the_model_and_the_advisor_with_their_own_rules_pass() -> None:
    assert check_policy.violations(MODEL, cots=1) == []


@pytest.mark.parametrize(
    "rule, expected",
    [
        ("allow llm_t self:ability { channel_connect:valved1_t };", "llm_t may connect to valved1_t"),
        ("allow llm_t self:ability { channel_connect:spo2d1_t };", "llm_t may connect to spo2d1_t"),
        ("allow llm_t self:ability { channel_connect:titractl1_t };", "llm_t may connect to titractl1_t"),
        ("allow llm_t self:ability { channel_connect:station_t };", "llm_t may connect to station_t"),
        ("allow llm_t self:ability { channel_connect:advisor_t };", "llm_t may connect to advisor_t"),
        ("allow llm_t self:ability { channel_connect:io_sock_t };", "llm_t may connect to io_sock_t"),
        ("allow advisor_t self:ability { channel_connect:valved1_t };", "advisor_t may connect to valved1_t"),
        ("allow advisor_t self:ability { channel_connect:io_sock_t };", "advisor_t may connect to io_sock_t"),
        ("allow station_t self:ability { channel_connect:llm_t };", "station_t may connect to llm_t"),
    ],
)
def test_a_path_out_of_the_model_or_the_advisor_is_a_violation(rule: str, expected: str) -> None:
    assert expected in check_policy.violations(MODEL + rule, cots=1)


@pytest.mark.parametrize(
    "ability, name",
    [
        ("spawn", "spawn"),
        ("priority:0-253", "priority"),
        ("mem_phys:0-4095", "mem_phys"),
        ("io:1", "io"),
        ("settypeid:titractl1_t", "settypeid"),
        ("rlimit:6", "rlimit"),
    ],
)
def test_an_ability_beyond_its_own_process_is_a_violation(ability: str, name: str) -> None:
    rule = f"allow llm_t self:ability {{ {ability} }};"
    assert f"llm_t holds the ability {name}" in check_policy.violations(MODEL + rule, cots=1)


def test_the_committed_policy_contains_the_model() -> None:
    text = (ROOT / "qnx" / "image" / "titra.secpol").read_text(encoding="utf-8")
    assert "type llm_t;" in text
    assert "type advisor_t;" in text
    assert check_policy.violations(text) == []
    assert "channel_connect" in check_policy.abilities_of(text, "llm_t")
