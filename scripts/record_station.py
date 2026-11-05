"""Records the nurse station as a sequence of frames during a live run, for the film.

    python scripts/record_station.py [--url http://127.0.0.1:8080/] [--every 2] [--seconds 3600] [--out DIR]

Each frame is a 2x JPEG named by the monotonic clock in milliseconds, the clock the live bench also uses
(bench.live records its own start on it), so frames can be matched to the fault events afterwards.
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8080/")
    ap.add_argument("--every", type=float, default=2.0)
    ap.add_argument("--seconds", type=float, default=3600.0)
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--out", default=str(ROOT / "data" / "film" / "station"))
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    end = time.monotonic() + a.seconds
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(viewport={"width": a.width, "height": 800}, device_scale_factor=2)
        page.goto(a.url)
        page.wait_for_selector("section.cot", timeout=60000)
        nxt = time.monotonic()
        while time.monotonic() < end:
            nxt += a.every
            stamp = int(time.monotonic() * 1000)
            try:
                page.screenshot(path=str(out / f"{stamp}.jpg"), type="jpeg", quality=88, full_page=True)
            except Exception as e:  # the board may restart a process mid-capture; the next frame retries
                print(f"frame skipped: {e}", flush=True)
            time.sleep(max(nxt - time.monotonic(), 0.0))
        browser.close()


if __name__ == "__main__":
    main()
