"""Trusted file adapters for the immutable GitHub Actions execution graph"""

import tomllib
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

from . import contracts
from . import suite as suite_model


def queue_metadata(
    command: dict[str, Any],
    *,
    batch_id: str,
    source_sha: str,
    now: datetime | None = None,
    defer_threshold_seconds: int = 60,
    max_defer_seconds: int = 29 * 24 * 60 * 60,
) -> dict[str, Any]:
    """Describe whether an accepted command should wait in the Actions queue."""
    start = contracts.BatchDefinition.model_validate(
        command["payload"]["definition"]
    ).start
    evaluated_at = (now or datetime.now(UTC)).astimezone(UTC)
    delay = (start - evaluated_at).total_seconds()
    if delay > max_defer_seconds:
        raise ValueError("definition.start_utc exceeds the 29-day queue horizon")
    return {
        "schema_version": 1,
        "batch_id": batch_id,
        "source_sha": source_sha,
        "start_utc": contracts.instant(start),
        "deferred": delay > defer_threshold_seconds,
    }


def _planner_inputs(
    definition: contracts.BatchDefinition,
    *,
    actor: str,
    identity: dict[str, Any],
    repository: str,
    ref: str,
    evaluated_at: datetime,
    runner_capacity: int,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """One validate_batch command and the idle fleet snapshot it plans against"""
    wire = definition.model_dump(mode="json")
    snapshot_version = f"fleet_{contracts.sha256_json(identity)[:20]}"
    command_digest = contracts.sha256_json(
        {"snapshot": snapshot_version, "definition": wire}
    )
    command = {
        "schema_version": 1,
        "command_id": f"command_{command_digest[:20]}",
        "operation": "validate_batch",
        "idempotency_key": f"{actor}-{command_digest[:24]}",
        "repository": {"id": repository, "ref": ref},
        "payload": {"definition": wire, "base_snapshot_version": snapshot_version},
    }
    snapshot_end = definition.start + timedelta(hours=definition.window_hours + 1)
    snapshot = {
        "schemaVersion": 1,
        "version": snapshot_version,
        "window": {
            "startsAt": contracts.instant(evaluated_at),
            "endsAt": contracts.instant(snapshot_end),
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
    }
    return command, snapshot


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
    suite = suite_model.load(suite_dir / f"{suite_id}.toml")
    if suite.meta.name != suite_id:
        raise ValueError(f"suite ID {suite_id!r} does not match {suite.meta.name!r}")
    evaluated_at = now.astimezone(UTC)
    definition = contracts.BatchDefinition(
        name=f"Repository {suite_id}",
        start_utc=evaluated_at + timedelta(minutes=5),
        window_hours=suite.meta.budget_hours,
        runners=suite.meta.runners,
        tests=[suite_id],
    )
    return _planner_inputs(
        definition,
        actor="repository",
        identity={
            "repository": repository,
            "evaluated_at": contracts.instant(evaluated_at),
            "suite": suite_id,
            "capacity": runner_capacity,
        },
        repository=repository,
        ref=ref,
        evaluated_at=evaluated_at,
        runner_capacity=runner_capacity,
    )


def inline_command(
    *,
    toml_text: str,
    repository: str,
    ref: str,
    now: datetime,
    runner_capacity: int = 12,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Translate inline TOML batch definition into planner input files."""
    try:
        batch = tomllib.loads(toml_text).get("batch")
    except tomllib.TOMLDecodeError as error:
        raise ValueError(f"invalid batch TOML: {error}") from error
    if not isinstance(batch, dict):
        raise ValueError("batch TOML missing [batch] table")
    evaluated_at = now.astimezone(UTC)
    definition = contracts.BatchDefinition.model_validate(
        {"start_utc": evaluated_at + timedelta(minutes=5), **batch}
    )
    return _planner_inputs(
        definition,
        actor="adhoc",
        identity={
            "repository": repository,
            "evaluated_at": contracts.instant(evaluated_at),
            "ad_hoc": True,
            "name": definition.name,
            "capacity": runner_capacity,
        },
        repository=repository,
        ref=ref,
        evaluated_at=evaluated_at,
        runner_capacity=runner_capacity,
    )


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
