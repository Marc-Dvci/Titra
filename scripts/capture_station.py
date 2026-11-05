"""Captures the nurse station served by the QNX guest, at 2x, for the README and the gallery.

    python scripts/capture_station.py [--url http://127.0.0.1:8080/] [--out docs/station.png] [--width 1280]

Waits until the page shows every cot and a trend, then saves a full-page PNG.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8080/")
    ap.add_argument("--out", default=str(ROOT / "docs" / "station.png"))
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--wait", type=float, default=6.0, help="seconds of history to let the trends draw")
    ap.add_argument("--page", help="serve this local index.html in place of the board's (previews a change)")
    a = ap.parse_args()
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": a.width, "height": 720}, device_scale_factor=2)
        if a.page:
            html = Path(a.page).read_text(encoding="utf-8")
            page.route(a.url, lambda route: route.fulfill(body=html, content_type="text/html"))
        page.goto(a.url)
        page.wait_for_selector("section.cot svg path", timeout=30000)
        page.wait_for_timeout(int(a.wait * 1000))
        Path(a.out).parent.mkdir(parents=True, exist_ok=True)
        page.screenshot(path=a.out, full_page=True)
        browser.close()
    print(a.out)


if __name__ == "__main__":
    main()
