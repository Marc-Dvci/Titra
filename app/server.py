"""The HTTP API, and the web app on the same port.

python -m uvicorn app.server:app --port 8787            # live features need MODEL_API_KEY
APP_DEMO=1 python -m uvicorn app.server:app --port 8787  # demo mode: no keys
"""

from __future__ import annotations

import os
from pathlib import Path

from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from . import __version__
from .core import Observation, decide

DEMO = os.environ.get("APP_DEMO") == "1"
WEB_DIR = Path(os.environ.get("APP_WEB_DIR") or Path(__file__).resolve().parents[1] / "web")
# TEMPLATE: the recorded answer demo mode serves; record a real one from a live run.
RECORDED_ANSWER = {"answer": "A recorded answer from a live run.", "recorded": True}

app = FastAPI(title="Titra", version=__version__)


class ObservationIn(BaseModel):
    source: str = Field(min_length=1)
    says: bool
    quality: float = Field(ge=0, le=1)


class Question(BaseModel):
    text: str = Field(min_length=1, max_length=2000)


@app.get("/api/health")
def health() -> dict[str, object]:
    return {"status": "ok", "version": __version__, "demo": DEMO}


@app.post("/api/decide")
def decide_route(observations: list[ObservationIn]) -> dict[str, object]:
    d = decide([Observation(o.source, o.says, o.quality) for o in observations])
    return {"outcome": d.outcome, "reason": d.reason, "counted": list(d.counted)}


@app.post("/api/ask")
def ask(question: Question) -> dict[str, object]:
    """A model-backed route: in demo mode it serves a recorded answer, and without a key it says what to do."""
    if DEMO:
        return RECORDED_ANSWER
    if not os.environ.get("MODEL_API_KEY"):
        raise HTTPException(503, "no model access: set MODEL_API_KEY in .env, or run with APP_DEMO=1")
    raise HTTPException(501, "TEMPLATE: call the model here")


# Mounted last, so /api/* wins.
if WEB_DIR.is_dir():
    app.mount("/", StaticFiles(directory=WEB_DIR, html=True), name="web")
