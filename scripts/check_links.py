"""Every relative link and image in the Markdown files points at a file that exists, and every #anchor at a heading.

    python scripts/check_links.py

Offline and deterministic, so it runs in CI. External links are checked in a browser before submitting.
Links that still hold a placeholder are left to scripts/check_placeholders.py.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LINK = re.compile(
    r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)|<img[^>]*\ssrc=\"([^\"]+)\"|<source[^>]*\ssrcset=\"([^\"]+)\""
)


def markdown_files() -> list[Path]:
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "*.md"], capture_output=True, text=True, check=True)
        return [ROOT / line for line in out.stdout.splitlines() if line]
    except (OSError, subprocess.CalledProcessError):
        return [p for p in ROOT.rglob("*.md") if ".venv" not in p.parts and "node_modules" not in p.parts]


def visible(text: str) -> str:
    """The text GitHub renders as links: no HTML comments, no fenced or inline code."""
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    return re.sub(r"`[^`\n]*`", "", text)


def slug(heading: str) -> str:
    """GitHub's anchor for a heading."""
    heading = re.sub(r"[`*_]|<[^>]+>", "", heading).strip().lower()
    heading = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)
    return re.sub(r"[^\w\- ]", "", heading).replace(" ", "-")


def anchors(path: Path) -> set[str]:
    text = re.sub(r"```.*?```", "", path.read_text(encoding="utf-8"), flags=re.S)
    return {slug(h) for h in re.findall(r"^#{1,6}\s+(.+?)\s*#*$", text, flags=re.M)}


def main() -> int:
    broken = []
    for md in markdown_files():
        for m in LINK.finditer(visible(md.read_text(encoding="utf-8"))):
            target = next(g for g in m.groups() if g)
            if "{{" in target or re.match(r"^[a-z][a-z0-9+.-]*:", target, flags=re.I):
                continue
            path, _, frag = target.partition("#")
            dest = (md.parent / path).resolve() if path else md
            if not dest.exists():
                broken.append(f"{md.relative_to(ROOT)}: {target} (no such file)")
            elif frag and dest.suffix == ".md" and frag.lower() not in anchors(dest):
                broken.append(f"{md.relative_to(ROOT)}: {target} (no such heading)")
    for line in broken:
        print(line)
    print(f"{len(broken)} broken link(s)")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
