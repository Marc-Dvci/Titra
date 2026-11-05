# Titra

**Open oxygen control for preterm babies on low-cost CPAP**

[![Tests](https://github.com/Marc-Dvci/Titra/actions/workflows/tests.yml/badge.svg)](https://github.com/Marc-Dvci/Titra/actions/workflows/tests.yml)
[![Decision core coverage](https://img.shields.io/badge/decision%20core%20coverage-{{COVERAGE}}%25-brightgreen)](tests/)
[![CodeQL](https://github.com/Marc-Dvci/Titra/actions/workflows/codeql.yml/badge.svg)](https://github.com/Marc-Dvci/Titra/actions/workflows/codeql.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![Live demo](https://img.shields.io/badge/live-demo-0a7d6e)]({{LIVE_URL}})

[![Watch the film: Titra](docs/film_thumbnail.jpg)](https://www.youtube.com/watch?v={{VIDEO_ID}})

**[Watch the film](https://www.youtube.com/watch?v={{VIDEO_ID}})**

<!-- TEMPLATE: one human line from Marc, confirmed facts only. -->

| Live demo | Track | Built with | Headline result |
|---|---|---|---|
| [Open it]({{LIVE_URL}}) | {{TRACK}} | {{SPONSOR}} | {{HEADLINE}} |

[For judges](#for-judges) · [See it work](#see-it-work) · [How it works](#how-it-works) · [Results](#results) ·
[Run it](#run-it)

## For judges

<!-- TEMPLATE: one row per judging criterion, in the rules' order. Link code to exact lines at the release tag
(blob/v1.0.0/<path>#L10-L40), and open every link once the tag exists. -->

| Criterion | Where it is answered |
|---|---|
| {{CRITERION}} | [How it works](#how-it-works), [`app/core.py`](app/core.py) |

## See it work

<!-- TEMPLATE: one GIF or screenshot per key moment, from the real product (scripts/make_gif.py). -->

## The problem

<!-- TEMPLATE: who it is for, the moment it matters, and the pain with one sourced number[^1]. -->

## How it works

![Titra, step by step: the input, the model step that reads it, the decision by fixed rules, and the output with
its evidence](docs/architecture.png)

Models read and phrase; plain code decides ([`app/core.py`](app/core.py)). The diagram's source is
[`docs/architecture.mmd`](docs/architecture.mmd).

## How {{SPONSOR}} is used

| Product | What it does here | Why it is needed |
|---|---|---|

## What it will not do

<!-- TEMPLATE: briefly: safety, approvals, refusals. -->

## Results

<!-- TEMPLATE: counts, before and after, and the command that reproduces each one. Then one line on what is real and
what is synthetic. -->

## Run it

> [!TIP]
> Nothing to install: the [live demo]({{LIVE_URL}}), or a ready environment in your browser:
> [![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/Marc-Dvci/Titra?quickstart=1)

### With Docker

| | Command | Opens at | Needs |
|---|---|---|---|
| **Demo** | `docker compose up demo` | http://localhost:8080 | Nothing |
| **Live** | `docker compose up app` | http://localhost:8787 | `cp .env.example .env`, then your keys |

### From source

Python 3.12.

```sh
python -m venv .venv
. .venv/Scripts/activate           # Linux, macOS: . .venv/bin/activate; PowerShell: .venv\Scripts\Activate.ps1
pip install -r requirements.txt
APP_DEMO=1 python -m uvicorn app.server:app --port 8787    # http://localhost:8787, no keys
```

Tests: `pip install -r requirements-dev.txt`, then `python -m pytest --cov`. {{TEST_COUNT}} tests run in a new random
order each time, each within 30 seconds; they cover {{COVERAGE}}% of the decision core's lines and
{{BRANCH_COVERAGE}}% of its branches, and CI fails below 90%. The CI run page splits them into unit, integration and
end-to-end tests.

## Claims

| Claim | Code | Test |
|---|---|---|
| A fact is settled only when two independent sources agree | [`app/core.py`](app/core.py) | [`tests/test_core.py`](tests/test_core.py) |

## Layout

- `app/` the decision core (`core.py`) and the HTTP API (`server.py`)
- `web/` the web page, served by the API on the same port
- `tests/` the rules and the HTTP entry points
- `docs/` the architecture diagram and its source
- `scripts/` checks (links, placeholders) and figure tools (diagram, GIFs)

## License

[MIT](LICENSE).

[^1]: {{SOURCE}}
