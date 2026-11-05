"""Checks Titra's security policy for the isolation the spec promises.

    python qnx/image/check_policy.py [qnx/image/titra.secpol]

The rules checked, for every cot N:
  - only titractlN_t may connect to valvedN_t (the blender driver of cot N);
  - only titractlN_t may connect to spo2dN_t (the oximeter driver of cot N);
  - station_t may connect to no blender or oximeter driver.
And for the language model:
  - llm_t may connect to none of Titra's processes and not to the network;
  - llm_t holds none of the abilities that could reach beyond its own process;
  - advisor_t may connect only to the controllers and to llm_t among Titra's processes, and not to the network;
  - only advisor_t and the root shell's type may connect to llm_t.
Both rule forms count: "allow A B:channel connect;" and "channel_connect:B" inside "allow A self:ability {...}".
Exit status 1 and one line per violation if a rule breaks the isolation.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT = HERE / "titra.secpol"


def connections(text: str) -> set[tuple[str, str]]:
    """Every (client type, channel type) pair the policy allows to connect."""
    text = re.sub(r"#[^\n]*", "", text)
    pairs: set[tuple[str, str]] = set()
    for m in re.finditer(r"allow\s+(\w+)\s+(\w+)\s*:\s*channel\s+connect\s*;", text):
        pairs.add((m.group(1), m.group(2)))
    for m in re.finditer(r"allow\s+(\w+)\s+self\s*:\s*ability\s*\{(.*?)\}\s*;", text, re.S):
        for cm in re.finditer(r"channel_connect\s*:\s*([\w\s,]+)", m.group(2)):
            for target in cm.group(1).replace("\n", " ").split(","):
                target = target.strip().split()[0] if target.strip() else ""
                if target:
                    pairs.add((m.group(1), target))
    return pairs


# Abilities that would take the model's process beyond itself.
LLM_FORBIDDEN_ABILITIES = (
    "spawn",
    "fork",
    "priority",
    "mem_phys",
    "io",
    "interrupt",
    "settypeid",
    "xprocess_query",
    "xprocess_debug",
    "xprocess_mem_read",
    "rlimit",
    "able_create",
    "reboot",
)


def abilities_of(text: str, type_name: str) -> set[str]:
    """Every ability name the policy gives a type, without its range."""
    text = re.sub(r"#[^\n]*", "", text)
    names: set[str] = set()
    for m in re.finditer(rf"allow\s+{re.escape(type_name)}\s+self\s*:\s*ability\s*\{{(.*?)\}}\s*;", text, re.S):
        for word in m.group(1).split():
            names.add(word.split(":")[0])
    return names


def model_violations(text: str, pairs: set[tuple[str, str]]) -> list[str]:
    out = []
    has_model = "llm_t" in {c for c, _ in pairs} or re.search(r"\btype\s+llm_t\s*;", text) is not None
    if not has_model:
        return out
    titra = re.compile(r"(valved|spo2d|titractl)\d+_t|station_t|advisor_t")
    for client, channel in sorted(pairs):
        if client == "llm_t" and (titra.fullmatch(channel) or channel == "io_sock_t"):
            out.append(f"llm_t may connect to {channel}")
        if channel == "llm_t" and client not in ("advisor_t", "user_root_t"):
            out.append(f"{client} may connect to llm_t")
        if client == "advisor_t" and (
            re.fullmatch(r"(valved|spo2d)\d+_t|station_t", channel) or channel == "io_sock_t"
        ):
            out.append(f"advisor_t may connect to {channel}")
    for name in sorted(abilities_of(text, "llm_t") & set(LLM_FORBIDDEN_ABILITIES)):
        out.append(f"llm_t holds the ability {name}")
    return out


def violations(text: str, cots: int = 3) -> list[str]:
    pairs = connections(text)
    out = model_violations(text, pairs)
    for client, channel in sorted(pairs):
        m = re.fullmatch(r"(valved|spo2d)(\d+)_t", channel)
        if m and client != f"titractl{m.group(2)}_t":
            out.append(f"{client} may connect to {channel}")
    for n in range(1, cots + 1):
        if (f"titractl{n}_t", f"valved{n}_t") not in pairs:
            out.append(f"titractl{n}_t has no rule to reach valved{n}_t")
    return out


def main() -> int:
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT
    problems = violations(path.read_text(encoding="utf-8"))
    for p in problems:
        print(p)
    if not problems:
        print(
            f"{path.name}: only each cot's own controller may reach that cot's drivers, and the model reaches none of them"
        )
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
