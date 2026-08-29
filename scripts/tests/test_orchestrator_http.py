from datetime import UTC, datetime

from charm.orchestrator.coordinator import Coordinator
from charm.orchestrator.http import HttpApplication, HttpRequest
from charm.orchestrator.operator import Operator, OperatorConfig
from charm.orchestrator.planner import Planner
from charm.orchestrator.store import MemoryStore
from charm.orchestrator.transport import MemoryTransport
from charm.paths import nightmare_dir

TOKEN = "staging-orchestrator-token"
NOW = datetime(2026, 8, 29, tzinfo=UTC)


def application() -> tuple[HttpApplication, MemoryStore]:
    store = MemoryStore(clock=lambda: NOW)
    operator = Operator(
        Coordinator(store, MemoryTransport()),
        Planner(nightmare_dir() / "suites"),
        OperatorConfig(
            "charmos",
            "axvonx/charmos",
            "axvonx",
            "charmos",
            "main",
            ("main",),
            "56bc1dfdd14a0b3e03ff1d166e7af98bec8cbb6f",
            "ghcr.io/axvonx/charmos-x86-env@sha256:" + "2" * 64,
            4,
        ),
    )
    return HttpApplication(operator, TOKEN), store


def headers(key: str = "") -> dict[str, str]:
    return {
        "authorization": f"Bearer {TOKEN}",
        "x-charmos-actor-id": "42",
        "x-charmos-actor-login": "operator",
        "x-request-id": "request-http-one",
        "idempotency-key": key,
    }


def test_http_adapter_serves_the_canonical_read_and_validate_shapes() -> None:
    app, store = application()
    fleet = app.handle(
        HttpRequest(
            "GET",
            "/fleet",
            {
                "repositoryId": "charmos",
                "ref": "main",
                "startsAt": "2026-08-29T00:00:00Z",
                "endsAt": "2026-08-30T00:00:00Z",
            },
            headers(),
        )
    )
    assert fleet.status == 200
    key = "validate-http-one"
    body = {
        "definition": {
            "name": "HTTP smoke",
            "startUtc": "2026-08-29T01:00:00Z",
            "windowHours": 1,
            "runners": 1,
            "color": "#a7c080",
            "tests": ["harness_smoke"],
        },
        "definitionToml": (
            "[batch]\n"
            'name = "HTTP smoke"\n'
            'start_utc = "2026-08-29T01:00:00Z"\n'
            "window_hours = 1\n"
            "runners = 1\n"
            'color = "#a7c080"\n'
            'tests = ["harness_smoke"]\n'
        ),
        "baseSnapshotVersion": fleet.body["version"],
        "idempotencyKey": key,
    }
    response = app.handle(
        HttpRequest("POST", "/batches/validate", {}, headers(key), body)
    )

    assert response.status == 200
    assert response.body["kind"] == "accepted"
    command = next(iter(store.read().state.commands.values()))
    assert command.request_id == "request-http-one"
    assert command.document["actor"]["login"] == "operator"


def test_http_adapter_rejects_unauthenticated_and_noncanonical_mutations() -> None:
    app, _store = application()
    unauthorized = app.handle(HttpRequest("GET", "/fleet", {}, {}))
    assert unauthorized.status == 401

    malformed = app.handle(
        HttpRequest(
            "POST",
            "/batches/submit",
            {},
            headers("header-key"),
            {
                "planId": "plan_one",
                "planVersion": "planv_one",
                "baseSnapshotVersion": "fleet_one",
                "idempotencyKey": "different-key",
            },
        )
    )
    assert malformed.status == 400
    assert malformed.body["code"] == "invalid_request"
