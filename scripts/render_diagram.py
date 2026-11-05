"""Render a Mermaid source to a committed PNG for the README (default: docs/architecture.mmd).

    python scripts/render_diagram.py [docs/architecture.mmd]

A committed image loads wherever the README is read; GitHub's own Mermaid renderer can refuse a diagram that other
Mermaid versions draw. Needs Playwright with Chromium: pip install -r requirements-dev.txt, then
playwright install chromium. Keep the result about 850 px wide or less, so its text stays readable on GitHub.
"""

from __future__ import annotations

import sys
from pathlib import Path

from playwright.sync_api import sync_playwright

ROOT = Path(__file__).resolve().parents[1]
MERMAID = "https://cdn.jsdelivr.net/npm/mermaid@11.4.1/dist/mermaid.min.js"
THEME = {  # white and grey, like the other figures
    "background": "#ffffff",
    "primaryColor": "#ffffff",
    "primaryBorderColor": "#57606a",
    "primaryTextColor": "#1f2328",
    "lineColor": "#57606a",
    "clusterBkg": "#f6f8fa",
    "clusterBorder": "#d0d7de",
    "fontFamily": "Segoe UI, Helvetica, Arial, sans-serif",
    "fontSize": "16px",
}
INIT = (
    "theme => { mermaid.initialize({ startOnLoad: false, theme: 'base', themeVariables: theme, "
    "flowchart: { useMaxWidth: false, wrappingWidth: 420, subGraphTitleMargin: { top: 0, bottom: 6 } } }); "
    "return mermaid.run(); }"
)


def main() -> None:
    src = (Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "docs/architecture.mmd").resolve()
    out = src.with_suffix(".png")
    code = "\n".join(line for line in src.read_text(encoding="utf-8").splitlines() if not line.startswith("%%"))
    with sync_playwright() as p:
        browser = p.chromium.launch()
        page = browser.new_page(device_scale_factor=2, viewport={"width": 1200, "height": 1400})
        page.set_content(
            '<body style="margin:0;background:#fff"><div id="d" style="display:inline-block;padding:24px">'
            '<pre class="mermaid"></pre></div></body>'
        )
        page.eval_on_selector("pre.mermaid", "(e, code) => { e.textContent = code; }", code)
        page.add_script_tag(url=MERMAID)
        page.evaluate(INIT, THEME)
        page.wait_for_selector("pre.mermaid svg")
        page.locator("#d").screenshot(path=str(out))
        width = page.locator("#d").bounding_box()["width"]
        browser.close()
    print(f"wrote {out.relative_to(ROOT)} ({width:.0f} px wide at 1x; GitHub shows about 850 px)")


if __name__ == "__main__":
    main()
