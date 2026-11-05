"""List what is still to fill: `{{PLACEHOLDER}}` values and `TEMPLATE:` notes.

python scripts/check_placeholders.py            # list them (CI shows them as warnings)
python scripts/check_placeholders.py --strict   # also fail if any is left: run it before submitting
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATTERN = re.compile(r"\{\{[A-Z_]+\}\}|TEMPLATE:")
SKIP = {"scripts/check_placeholders.py", "scripts/init_project.py", "TEMPLATE.md"}


def tracked_files() -> list[str]:
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "ls-files"], capture_output=True, text=True, check=True).stdout
        return [line for line in out.splitlines() if line]
    except (OSError, subprocess.CalledProcessError):
        skip = {".git", ".venv", "node_modules", "__pycache__"}
        return [
            p.relative_to(ROOT).as_posix()
            for p in ROOT.rglob("*")
            if p.is_file() and not skip & set(p.relative_to(ROOT).parts)
        ]


def main() -> int:
    found = 0
    in_ci = os.environ.get("GITHUB_ACTIONS") == "true"
    for rel in tracked_files():
        if rel in SKIP:
            continue
        try:
            lines = (ROOT / rel).read_text(encoding="utf-8").splitlines()
        except (UnicodeDecodeError, OSError):
            continue
        for n, line in enumerate(lines, 1):
            for m in PATTERN.finditer(line):
                found += 1
                note = line.strip()[:120]
                print(f"::warning file={rel},line={n}::{m.group(0)} {note}" if in_ci else f"{rel}:{n}: {note}")
    print(f"{found} placeholder(s) or TEMPLATE note(s) left")
    return 1 if found and "--strict" in sys.argv else 0


if __name__ == "__main__":
    sys.exit(main())
