"""The HTTP entry points, through the test client: what a caller actually reaches."""

import pytest
from fastapi.testclient import TestClient

from app import __version__, server

pytestmark = pytest.mark.integration

client = TestClient(server.app)


def test_health_reports_the_version_and_the_mode() -> None:
    assert client.get("/api/health").json() == {"status": "ok", "version": __version__, "demo": server.DEMO}


def test_the_decide_route_applies_the_rules() -> None:
    body = [{"source": "satellite", "says": True, "quality": 0.9}, {"source": "registry", "says": True, "quality": 0.8}]
    r = client.post("/api/decide", json=body)
    assert r.status_code == 200
    assert r.json() == {
        "outcome": "present",
        "reason": "2 independent sources agree",
        "counted": ["registry", "satellite"],
    }


def test_a_quality_outside_zero_to_one_is_refused() -> None:
    r = client.post("/api/decide", json=[{"source": "satellite", "says": True, "quality": 1.5}])
    assert r.status_code == 422


def test_without_a_key_a_model_route_says_what_to_do(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(server, "DEMO", False)
    monkeypatch.delenv("MODEL_API_KEY", raising=False)
    r = client.post("/api/ask", json={"text": "Is it there?"})
    assert r.status_code == 503
    assert "MODEL_API_KEY" in r.json()["detail"]


def test_demo_mode_answers_without_a_key(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(server, "DEMO", True)
    monkeypatch.delenv("MODEL_API_KEY", raising=False)
    r = client.post("/api/ask", json={"text": "Is it there?"})
    assert (r.status_code, r.json()["recorded"]) == (200, True)


def test_the_web_app_is_served_on_the_same_port() -> None:
    r = client.get("/")
    assert r.status_code == 200
    assert "<title>" in r.text
