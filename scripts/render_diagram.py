"""Render the architecture figure to the committed PNG the README shows (docs/architecture.html, 1200 x 800 at 2x).

    python scripts/render_diagram.py

A committed image loads wherever the README is read. Needs Playwright with Chromium: pip install -r requirements-dev.txt,
then playwright install chromium.
"""

from __future__ import annotations

from pathlib import Path

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    src = ROOT / "docs" / "architecture.html"
    out = src.with_suffix(".png")
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(device_scale_factor=2, viewport={"width": 1200, "height": 800})
        page.goto(src.as_uri())
        page.screenshot(path=str(out))
        browser.close()
    print(f"wrote {out.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
