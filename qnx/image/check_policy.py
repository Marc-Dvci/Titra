"""Checks Titra's security policy for the isolation the spec promises.

    python qnx/image/check_policy.py [qnx/image/titra.secpol]

The rules checked, for every cot N:
  - only titractlN_t may connect to valvedN_t (the blender driver of cot N);
  - only titractlN_t may connect to spo2dN_t (the oximeter driver of cot N);
  - station_t may connect to no blender or oximeter driver.
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


def violations(text: str, cots: int = 3) -> list[str]:
    pairs = connections(text)
    out = []
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
        print(f"{path.name}: only each cot's own controller may reach that cot's drivers")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
