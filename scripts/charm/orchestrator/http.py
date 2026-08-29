"""Minimal authenticated HTTP adapter for the CI Studio operator interface."""

from __future__ import annotations

import hmac
import json
import os
from dataclasses import dataclass
from http import HTTPStatus
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs

from .coordinator import Coordinator
from .operator import Audit, FleetQuery, Operator, OperatorConfig, OperatorError
from .planner import Planner
from .store import PostgresStore
from .transport import GitHubActionsTransport


@dataclass(frozen=True)
class HttpRequest:
    method: str
    path: str
    query: dict[str, str]
    headers: dict[str, str]
    body: Any = None


@dataclass(frozen=True)
class HttpResponse:
    status: int
    body: dict[str, Any]


class HttpApplication:
    """Translate HTTP mechanics into the six-operation domain interface."""

    def __init__(self, operator: Operator, bearer_token: str) -> None:
        if len(bearer_token) < 16:
            raise ValueError(
                "the orchestrator bearer token must be at least 16 characters"
            )
        self.operator = operator
        self.bearer_token = bearer_token

    def handle(self, request: HttpRequest) -> HttpResponse:
        try:
            audit = self._authorize(request)
            return self._dispatch(request, audit)
        except _HttpFailure as error:
            return error.response
        except OperatorError as error:
            return HttpResponse(
                HTTPStatus.BAD_REQUEST,
                {"code": "invalid_request", "message": str(error)},
            )

    def _authorize(self, request: HttpRequest) -> Audit:
        authorization = request.headers.get("authorization", "")
        expected = f"Bearer {self.bearer_token}"
        if not hmac.compare_digest(authorization, expected):
            raise _HttpFailure(
                HTTPStatus.UNAUTHORIZED,
                "unauthorized",
                "A valid server credential is required.",
            )
        return Audit(
            request.headers.get("x-charmos-actor-id", ""),
            request.headers.get("x-charmos-actor-login", ""),
            request.headers.get("x-request-id", ""),
        )

    def _dispatch(self, request: HttpRequest, audit: Audit) -> HttpResponse:
        method = request.method.upper()
        path = request.path.strip("/")
        if method == "GET" and path == "fleet":
            snapshot = self.operator.get_fleet_window(FleetQuery.parse(request.query))
            return HttpResponse(HTTPStatus.OK, snapshot)
        if method == "GET" and path.startswith("batches/") and path.count("/") == 1:
            batch = self.operator.get_batch(path.split("/", 1)[1])
            if batch is None:
                raise _HttpFailure(
                    HTTPStatus.NOT_FOUND,
                    "unknown_batch",
                    "The requested batch does not exist.",
                )
            return HttpResponse(HTTPStatus.OK, batch)
        if method != "POST" or not isinstance(request.body, dict):
            raise _HttpFailure(
                HTTPStatus.NOT_FOUND,
                "unknown_operation",
                "This orchestrator operation is not available.",
            )
        body = request.body
        header_key = request.headers.get("idempotency-key", "")
        if body.get("idempotencyKey") != header_key:
            raise OperatorError("idempotency-key header must match the command body")
        if path == "batches/validate":
            _validate_body(body)
            return HttpResponse(
                HTTPStatus.OK, self.operator.validate_batch(body, audit)
            )
        if path == "batches/submit":
            _submit_body(body)
            return HttpResponse(HTTPStatus.OK, self.operator.submit_batch(body, audit))
        if path.startswith("batches/") and path.endswith("/tail/draft"):
            batch_id = path.removeprefix("batches/").removesuffix("/tail/draft")
            _tail_draft_body(body)
            if body["batchId"] != batch_id:
                raise OperatorError("batchId does not match the route")
            return HttpResponse(
                HTTPStatus.OK, self.operator.draft_tail_update(body, audit)
            )
        if path == "batches/tail/commit":
            _tail_commit_body(body)
            return HttpResponse(
                HTTPStatus.OK, self.operator.commit_tail_update(body, audit)
            )
        raise _HttpFailure(
            HTTPStatus.NOT_FOUND,
            "unknown_operation",
            "This orchestrator operation is not available.",
        )

    def __call__(self, environ: dict[str, Any], start_response: Any) -> list[bytes]:
        try:
            length = int(environ.get("CONTENT_LENGTH") or 0)
            if length > 1_048_576:
                response = HttpResponse(
                    HTTPStatus.REQUEST_ENTITY_TOO_LARGE,
                    {
                        "code": "request_too_large",
                        "message": "The request body is too large.",
                    },
                )
            else:
                raw = environ["wsgi.input"].read(length) if length else b""
                body = json.loads(raw) if raw else None
                headers = {
                    key[5:].lower().replace("_", "-"): str(value)
                    for key, value in environ.items()
                    if key.startswith("HTTP_")
                }
                request = HttpRequest(
                    str(environ.get("REQUEST_METHOD", "GET")),
                    str(environ.get("PATH_INFO", "/")),
                    {
                        key: values[-1]
                        for key, values in parse_qs(
                            str(environ.get("QUERY_STRING", "")),
                            keep_blank_values=True,
                        ).items()
                    },
                    headers,
                    body,
                )
                response = self.handle(request)
        except _HttpFailure as error:
            response = error.response
        except (json.JSONDecodeError, UnicodeDecodeError):
            response = HttpResponse(
                HTTPStatus.BAD_REQUEST,
                {
                    "code": "invalid_json",
                    "message": "The request body is not valid JSON.",
                },
            )
        payload = json.dumps(response.body, separators=(",", ":")).encode()
        start_response(
            f"{response.status} {HTTPStatus(response.status).phrase}",
            [
                ("content-type", "application/json"),
                ("content-length", str(len(payload))),
                ("cache-control", "no-store"),
            ],
        )
        return [payload]


class _HttpFailure(Exception):
    def __init__(self, status: int, code: str, message: str) -> None:
        self.response = HttpResponse(status, {"code": code, "message": message})


def _shape(document: dict[str, Any], required: set[str]) -> None:
    if set(document) != required:
        missing = sorted(required - set(document))
        unknown = sorted(set(document) - required)
        detail = []
        if missing:
            detail.append(f"missing {', '.join(missing)}")
        if unknown:
            detail.append(f"unknown {', '.join(unknown)}")
        raise OperatorError("invalid command shape: " + "; ".join(detail))


def _validate_body(document: dict[str, Any]) -> None:
    _shape(
        document,
        {"definition", "definitionToml", "baseSnapshotVersion", "idempotencyKey"},
    )
    definition = document["definition"]
    if not isinstance(definition, dict):
        raise OperatorError("definition must be an object")
    _shape(
        definition,
        {"name", "startUtc", "windowHours", "runners", "color", "tests"},
    )


def _submit_body(document: dict[str, Any]) -> None:
    _shape(
        document,
        {"planId", "planVersion", "baseSnapshotVersion", "idempotencyKey"},
    )


def _tail_draft_body(document: dict[str, Any]) -> None:
    _shape(
        document,
        {"batchId", "baseBatchVersion", "cancelledTaskIds", "idempotencyKey"},
    )
    cancelled = document["cancelledTaskIds"]
    if (
        not isinstance(cancelled, list)
        or not cancelled
        or not all(isinstance(item, str) for item in cancelled)
        or len(set(cancelled)) != len(cancelled)
    ):
        raise OperatorError("cancelledTaskIds must be a non-empty unique string array")


def _tail_commit_body(document: dict[str, Any]) -> None:
    _shape(
        document,
        {"planId", "planVersion", "baseBatchVersion", "idempotencyKey"},
    )


def create_application_from_env() -> HttpApplication:
    """Create the production WSGI adapter from server-only configuration."""
    token = os.environ.get("ORCHESTRATOR_TOKEN", "")
    if not token:
        raise RuntimeError("missing production configuration: ORCHESTRATOR_TOKEN")
    return HttpApplication(create_operator_from_env(), token)


def create_operator_from_env() -> Operator:
    """Create durable authority for the HTTP adapter or Actions coordinator."""
    required = {
        name: os.environ.get(name, "")
        for name in (
            "NIGHTMARE_DATABASE_DSN",
            "GITHUB_TOKEN",
            "NIGHTMARE_SOURCE_COMMIT",
            "NIGHTMARE_RUNNER_IMAGE",
        )
    }
    missing = [name for name, value in required.items() if not value]
    if missing:
        raise RuntimeError(f"missing production configuration: {', '.join(missing)}")
    repository = os.environ.get("NIGHTMARE_REPOSITORY", "axvonx/charmos")
    owner, name = repository.split("/", 1)
    default_ref = os.environ.get("NIGHTMARE_DEFAULT_REF", "main")
    refs = tuple(
        item.strip()
        for item in os.environ.get("NIGHTMARE_REFS", default_ref).split(",")
        if item.strip()
    )
    store = PostgresStore.from_dsn(required["NIGHTMARE_DATABASE_DSN"])
    store.migrate()
    transport = GitHubActionsTransport(
        repository=repository,
        workflow=os.environ.get("NIGHTMARE_WORKFLOW", "nightmare-orchestrator.yml"),
        ref=default_ref,
        token=required["GITHUB_TOKEN"],
    )
    return Operator(
        Coordinator(store, transport),
        Planner(Path(os.environ.get("NIGHTMARE_SUITE_DIR", "nightmare/suites"))),
        OperatorConfig(
            repository_id=os.environ.get("NIGHTMARE_REPOSITORY_ID", name),
            repository=repository,
            owner=owner,
            name=name,
            default_ref=default_ref,
            refs=refs,
            source_commit=required["NIGHTMARE_SOURCE_COMMIT"],
            runner_image=required["NIGHTMARE_RUNNER_IMAGE"],
            runner_capacity=int(os.environ.get("NIGHTMARE_RUNNER_CAPACITY", "10")),
        ),
    )
