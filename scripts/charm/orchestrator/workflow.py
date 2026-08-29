"""Trusted file adapters for the immutable GitHub Actions execution graph"""

from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

from ..nightmare import contracts
from ..nightmare import suite as suite_model


def _instant(value: datetime) -> str:
    return value.astimezone(UTC).isoformat().replace("+00:00", "Z")


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
