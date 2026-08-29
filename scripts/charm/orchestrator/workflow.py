"""Trusted file adapters for the immutable GitHub Actions execution graph"""

import json
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

from ..nightmare import contracts
from ..nightmare import suite as suite_model


def _instant(value: datetime) -> str:
    return value.astimezone(UTC).isoformat().replace("+00:00", "Z")


def wait_until(target_time: datetime, max_wait_seconds: int = 21600) -> float:
    """If target_time is in the future, sleep until target_time (up to max_wait_seconds)."""
    import time

    now = datetime.now(UTC)
    delay = (target_time.astimezone(UTC) - now).total_seconds()
    if delay <= 0:
        return 0.0
    if delay > max_wait_seconds:
        raise ValueError(
            f"target time {target_time.isoformat()} exceeds maximum wait window of {max_wait_seconds}s"
        )
    time.sleep(delay)
    return delay


def repository_command(
    *,
    suite_id: str,
    suite_dir: Path,
    repository: str,
    ref: str,
    now: datetime,
    runner_capacity: int = 12,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Translate one committed schedule selection into planner input files."""
    suite_path = suite_dir / f"{suite_id}.toml"
    suite = suite_model.load(suite_path)
    if suite.meta.name != suite_id:
        raise ValueError(f"suite ID {suite_id!r} does not match {suite.meta.name!r}")
    evaluated_at = now.astimezone(UTC)
    start = evaluated_at + timedelta(minutes=5)
    window_hours = suite.meta.budget_hours
    runners = suite.meta.runners
    snapshot_end = start + timedelta(hours=window_hours + 1)
    definition = {
        "name": f"Repository {suite_id}",
        "start_utc": _instant(start),
        "window_hours": window_hours,
        "runners": runners,
        "color": "#a7c080",
        "tests": [suite_id],
    }
    definition_toml = (
        "[batch]\n"
        f'name = "Repository {suite_id}"\n'
        f'start_utc = "{_instant(start)}"\n'
        f"window_hours = {window_hours}\n"
        f"runners = {runners}\n"
        'color = "#a7c080"\n'
        f'tests = ["{suite_id}"]\n'
    )
    snapshot_identity = {
        "repository": repository,
        "evaluated_at": _instant(evaluated_at),
        "suite": suite_id,
        "capacity": runner_capacity,
    }
    snapshot_version = f"fleet_{contracts.sha256_json(snapshot_identity)[:20]}"
    command_digest = contracts.sha256_json(
        {"snapshot": snapshot_version, "definition": definition}
    )
    command = {
        "schema_version": 1,
        "command_id": f"command_{command_digest[:20]}",
        "operation": "validate_batch",
        "idempotency_key": f"repository-{command_digest[:24]}",
        "actor": {"id": "repository-schedule", "login": "github-actions"},
        "repository": {"id": repository, "ref": ref},
        "payload": {
            "definition": definition,
            "definition_toml": definition_toml,
            "base_snapshot_version": snapshot_version,
        },
    }
    owner, name = repository.split("/", 1)
    snapshot = {
        "schemaVersion": 1,
        "version": snapshot_version,
        "evaluatedAt": _instant(evaluated_at),
        "window": {
            "startsAt": _instant(evaluated_at),
            "endsAt": _instant(snapshot_end),
        },
        "repository": {
            "id": name,
            "owner": owner,
            "name": name,
            "ref": ref,
            "refs": [ref],
        },
        "runners": [
            {
                "id": f"runner_{index}",
                "number": index,
                "state": "idle",
                "label": f"Runner {index}",
            }
            for index in range(1, runner_capacity + 1)
        ],
        "batches": [],
        "partial": False,
    }
    return command, snapshot


def inline_command(
    *,
    toml_text: str,
    repository: str,
    ref: str,
    now: datetime,
    runner_capacity: int = 12,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Translate inline TOML batch definition into planner input files."""
    import tomllib

    try:
        raw = tomllib.loads(toml_text)
    except tomllib.TOMLDecodeError as error:
        raise ValueError(f"invalid batch TOML: {error}") from error

    batch = raw.get("batch")
    if not isinstance(batch, dict):
        raise ValueError("batch TOML missing [batch] table")

    name = batch.get("name")
    if not isinstance(name, str) or not name.strip():
        raise ValueError("[batch].name must be a non-empty string")

    evaluated_at = now.astimezone(UTC)
    start_str = batch.get("start_utc")
    if isinstance(start_str, str):
        try:
            start = datetime.fromisoformat(start_str.replace("Z", "+00:00")).astimezone(
                UTC
            )
        except ValueError as error:
            raise ValueError(f"invalid start_utc: {error}") from error
    else:
        start = evaluated_at + timedelta(minutes=5)

    window_hours = batch.get("window_hours")
    if not isinstance(window_hours, int) or window_hours < 1:
        raise ValueError("[batch].window_hours must be a positive integer")

    runners = batch.get("runners")
    if not isinstance(runners, int) or runners < 1:
        raise ValueError("[batch].runners must be a positive integer")

    color = str(batch.get("color") or "#a7c080")
    tests = batch.get("tests")
    if (
        not isinstance(tests, list)
        or not tests
        or not all(isinstance(t, str) for t in tests)
    ):
        raise ValueError("[batch].tests must be a non-empty list of test/suite names")

    snapshot_end = start + timedelta(hours=window_hours + 1)
    definition = {
        "name": name,
        "start_utc": _instant(start),
        "window_hours": window_hours,
        "runners": runners,
        "color": color,
        "tests": tests,
    }
    tests_repr = json.dumps(tests)
    definition_toml = (
        "[batch]\n"
        f'name = "{name}"\n'
        f'start_utc = "{_instant(start)}"\n'
        f"window_hours = {window_hours}\n"
        f"runners = {runners}\n"
        f'color = "{color}"\n'
        f"tests = {tests_repr}\n"
    )

    snapshot_identity = {
        "repository": repository,
        "evaluated_at": _instant(evaluated_at),
        "ad_hoc": True,
        "name": name,
        "capacity": runner_capacity,
    }
    snapshot_version = f"fleet_{contracts.sha256_json(snapshot_identity)[:20]}"
    command_digest = contracts.sha256_json(
        {"snapshot": snapshot_version, "definition": definition}
    )
    command = {
        "schema_version": 1,
        "command_id": f"command_{command_digest[:20]}",
        "operation": "validate_batch",
        "idempotency_key": f"adhoc-{command_digest[:24]}",
        "actor": {"id": "adhoc-dispatch", "login": "github-actions"},
        "repository": {"id": repository, "ref": ref},
        "payload": {
            "definition": definition,
            "definition_toml": definition_toml,
            "base_snapshot_version": snapshot_version,
        },
    }
    owner, repo_name = (
        repository.split("/", 1) if "/" in repository else ("local", repository)
    )
    snapshot = {
        "schemaVersion": 1,
        "version": snapshot_version,
        "evaluatedAt": _instant(evaluated_at),
        "window": {
            "startsAt": _instant(evaluated_at),
            "endsAt": _instant(snapshot_end),
        },
        "repository": {
            "id": repo_name,
            "owner": owner,
            "name": repo_name,
            "ref": ref,
            "refs": [ref],
        },
        "runners": [
            {
                "id": f"runner_{index}",
                "number": index,
                "state": "idle",
                "label": f"Runner {index}",
            }
            for index in range(1, runner_capacity + 1)
        ],
        "batches": [],
        "partial": False,
    }
    return command, snapshot


def build_matrix(plan_result: dict[str, Any], limit: int = 64) -> list[dict[str, str]]:
    if plan_result.get("kind") != "accepted":
        return []
    groups = plan_result["plan"]["buildGroups"]
    if len(groups) > limit:
        raise ValueError(f"build matrix has {len(groups)} rows; limit is {limit}")
    return [
        {
            "bundle_id": group["id"],
            "artifact": f"nightmare-build-{group['id']}",
        }
        for group in sorted(groups, key=lambda item: item["id"])
    ]


def runner_matrix(plan_bundle: dict[str, Any], limit: int = 64) -> list[dict[str, str]]:
    rows = plan_bundle["matrix"]
    if len(rows) > limit:
        raise ValueError(f"runner matrix has {len(rows)} rows; limit is {limit}")
    return [
        {
            "manifest_id": row["manifest_id"],
            "manifest_artifact": row["manifest_artifact"],
            "bundle_id": row["bundle_id"],
            "bundle_artifact": f"nightmare-build-{row['bundle_id']}",
        }
        for row in sorted(rows, key=lambda item: item["manifest_id"])
    ]
